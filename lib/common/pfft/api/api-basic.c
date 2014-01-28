/*
 * Copyright (c) 2010-2013 Michael Pippig
 *
 * This file is part of PFFT.
 *
 * PFFT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PFFT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PFFT.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "pfft.h"
#include "ipfft.h"
#include "util.h"

#define DATA_INIT(i) (( (R)1000 ) / ( (R)( (i) == 0 ? 1 : i) ))

/* Global infos about procmesh are only enabled in debug mode
 * Otherwise we do not use any global variables. */
#if PFFT_DEBUG_GVARS
  MPI_Comm *gdbg_comm_cart;
  int gdbg_rnk_pm=-1;
  MPI_Comm *gdbg_comms_pm;
#endif

static void twiddle_input(
    PX(plan) ths);
static void twiddle_output(
    PX(plan) ths);

static void execute_transposed(
    int rnk_pm, outrafo_plan *trafos, gtransp_plan *remaps,
    double *timer_trafo, double *timer_remap);


static INT plain_index(
    int rnk, const INT *kvec, const INT *n);
static void vector_index(
    int rnk, INT k, const INT *n,
    INT *kvec);

static void complex_conjugate(
    R* data, const int rnk_n, const int rnk_pm, const INT *n, const MPI_Comm *comms_pm, INT *block);

/* wrappers for fftw init and cleanup */
void PX(init) (void){
  XM(init)();
}

void PX(cleanup) (void){
  XM(cleanup)();
}


void PX(destroy_gcplan) (
    PX(gcplan) ths
    )
{
  PX(rmplan_gc)(ths);
}


void PX(destroy_plan)(
    PX(plan) ths
    )
{
  if(ths==NULL){
    PX(fprintf)(MPI_COMM_WORLD, stderr, "!!! Error: Can not finalize PFFT Plan == NULL !!!\n");
    return;
  }

  PX(rmplan)(ths);
}




static INT plain_index(
    int rnk, const INT *kvec, const INT *n
    )
{
  INT k=0;

  for(INT t=0; t<rnk; t++)
    k = k*n[t] + kvec[t];

  return k;
}

  
static void vector_index(
    int rnk, INT k, const INT *n,
    INT *kvec
    )
{
  for(INT t=rnk-1; t>=0; t--){
    kvec[t] = k%n[t];
    k = (k - kvec[t])/n[t];
  }
}


static void complex_conjugate(
    R* data, const int rnk_n, const int rnk_pm, const INT *n, const MPI_Comm *comms_pm, INT *block
    )
{
  INT local_n_total = 1;

  for (int t=0; t<rnk_pm; t++)
    local_n_total *= PX(local_block_size_shifted)(n[t], block[t], 0, comms_pm[t]);
  for (int t=rnk_pm; t<rnk_n-1; t++)
    local_n_total *= n[t];

  local_n_total *= n[rnk_n-1]/2 + 1;

  for (INT k=0; k<local_n_total; k++)
    data[1+2*k] *= -1;
}


void PX(init_input_complex_3d)(
    const INT *n, const INT *local_n, const INT *local_start,
    C *data
    )
{
  int rnk_n=3;

  PX(init_input_complex)(rnk_n, n, local_n, local_start,
      data);
}

void PX(init_input_complex)(
    int rnk_n, const INT *n, const INT *local_n, const INT *local_start,
    C *data
    )
{ /* initialize FFT input with random numbers */
  INT m, ln_tot;
  INT *kvec_loc, *kvec_glob;

  kvec_loc  = PX(malloc_INT)(rnk_n);
  kvec_glob = PX(malloc_INT)(rnk_n);
 
  ln_tot = PX(prod_INT)(rnk_n, local_n);
  for(INT k=0; k<ln_tot; k++){
    vector_index(rnk_n, k, local_n, kvec_loc);
    PX(vadd_INT)(rnk_n, kvec_loc, local_start, kvec_glob);

    /* take care of possible fftshift, e.g., index runs from -n/2 to n/2-1 */
    for(int t=0; t<rnk_n; t++)
      if(kvec_glob[t] < 0)
        kvec_glob[t] += n[t];
    
    m = plain_index(rnk_n, kvec_glob, n);
    data[k][0] = DATA_INIT(2*m);
    data[k][1] = DATA_INIT(2*m+1);
  }

  free(kvec_loc); free(kvec_glob);
}

void PX(init_input_real_3d)(
    const INT *n, const INT *local_n, const INT *local_start,
    R *data
    )
{
  int rnk_n=3;

  PX(init_input_real)(rnk_n, n, local_n, local_start,
      data);
}

void PX(init_input_real)(
    int rnk_n, const INT *n, const INT *local_n, const INT *local_start,
    R *data
    )
{ 
  INT m, ln_tot;
  INT *kvec_loc, *kvec_glob;

  kvec_loc  = PX(malloc_INT)(rnk_n);
  kvec_glob = PX(malloc_INT)(rnk_n);
 
  ln_tot = PX(prod_INT)(rnk_n, local_n);
  for(INT k=0; k<ln_tot; k++){
    vector_index(rnk_n, k, local_n, kvec_loc);
    PX(vadd_INT)(rnk_n, kvec_loc, local_start, kvec_glob);
    
    m = plain_index(rnk_n, kvec_glob, n);
    data[k] = DATA_INIT(2*m);
  }

  free(kvec_loc); free(kvec_glob);
}



/* Check results after one forward and backward FFT.
 * Only works for intial data layout */
R PX(check_output_complex_3d)(
    const INT *n, const INT *local_n, const INT *local_start,
    const C *data, MPI_Comm comm
    )
{
  int rnk_n = 3; 

  return PX(check_output_complex)(rnk_n, n, local_n, local_start,
      data, comm);
}

R PX(check_output_complex)(
    int rnk_n, const INT *n, const INT *local_n, const INT *local_start,
    const C *data, MPI_Comm comm
    )
{ 
  INT m, ln_tot;
  INT *kvec_loc, *kvec_glob;
  R re, im, err, maxerr, globmaxerr;

  err = maxerr = 0;
  
  kvec_loc  = PX(malloc_INT)(rnk_n);
  kvec_glob = PX(malloc_INT)(rnk_n);
 
  ln_tot = PX(prod_INT)(rnk_n, local_n);
  for(INT k=0; k<ln_tot; k++){
    vector_index(rnk_n, k, local_n, kvec_loc);
    PX(vadd_INT)(rnk_n, kvec_loc, local_start, kvec_glob);
    
    m = plain_index(rnk_n, kvec_glob, n);
    re = data[k][0] - DATA_INIT(2*m);
    im = data[k][1] - DATA_INIT(2*m+1);
    err = pfft_sqrt(re*re + im*im);
    if( err > maxerr )
      maxerr = err;
  }

  free(kvec_loc); free(kvec_glob);

  MPI_Allreduce(&maxerr, &globmaxerr, 1, PFFT_MPI_REAL_TYPE, MPI_MAX, comm);
  return globmaxerr;
}

R PX(check_output_real_3d)(
    const INT *n, const INT *local_n, const INT *local_start,
    const R *data, MPI_Comm comm
    )
{
  int rnk_n = 3; 

  return PX(check_output_real)(rnk_n, n, local_n, local_start,
      data, comm);
}

R PX(check_output_real)(
    int rnk_n, const INT *n, const INT *local_n, const INT *local_start,
    const R *data, MPI_Comm comm
    )
{ 
  INT m, ln_tot;
  INT *kvec_loc, *kvec_glob;
  R re, im, err, maxerr, globmaxerr;

  err = maxerr = 0;
  
  kvec_loc  = PX(malloc_INT)(rnk_n);
  kvec_glob = PX(malloc_INT)(rnk_n);
 
  ln_tot = PX(prod_INT)(rnk_n, local_n);
  for(INT k=0; k<ln_tot; k++){
    vector_index(rnk_n, k, local_n, kvec_loc);
    PX(vadd_INT)(rnk_n, kvec_loc, local_start, kvec_glob);
    
    m = plain_index(rnk_n, kvec_glob, n);
    re = data[k] - DATA_INIT(2*m);
    im = 0;
    err = pfft_sqrt(re*re + im*im);
    if( err > maxerr )
      maxerr = err;
  }

  free(kvec_loc); free(kvec_glob);

  MPI_Allreduce(&maxerr, &globmaxerr, 1, PFFT_MPI_REAL_TYPE, MPI_MAX, comm);
  return globmaxerr;
}


INT PX(local_size_gc_3d)(
    const INT *local_n, const INT *local_start,
    INT alloc_local, const INT *gc_below, const INT *gc_above,
    INT *local_ngc, INT *local_gc_start
    )
{
  int rnk_n = 3;
  
  return PX(local_size_gc)(
      rnk_n, local_n, local_start, alloc_local, gc_below, gc_above,
      local_ngc, local_gc_start);
}

INT PX(local_size_gc)(
    int rnk_n, const INT *local_n, const INT *local_start,
    INT alloc_local, const INT *gc_below, const INT *gc_above,
    INT *local_ngc, INT *local_gc_start
    )
{
  INT howmany = 1;
  
  return PX(local_size_many_gc)(
      rnk_n, local_n, local_start, alloc_local, howmany, gc_below, gc_above,
      local_ngc, local_gc_start);
}



PX(gcplan) PX(plan_rgc_3d)(
    const INT *n, const INT *gc_below, const INT *gc_above,
    R *data, MPI_Comm comm_cart, unsigned gc_flags
    )
{
  int rnk_n = 3;
  
  return PX(plan_rgc)(rnk_n, n, gc_below, gc_above,
      data, comm_cart, gc_flags);
}

PX(gcplan) PX(plan_cgc_3d)(
    const INT *n, const INT *gc_below, const INT *gc_above,
    C *data, MPI_Comm comm_cart, unsigned gc_flags
    )
{
  int rnk_n = 3;
  
  return PX(plan_cgc)(rnk_n, n, gc_below, gc_above,
      data, comm_cart, gc_flags);
}

PX(gcplan) PX(plan_rgc)(
    int rnk_n, const INT *n, const INT *gc_below, const INT *gc_above,
    R *data, MPI_Comm comm_cart, unsigned gc_flags
    )
{
  INT howmany = 1, *block = PFFT_DEFAULT_BLOCKS;
  
  return PX(plan_many_rgc)(
      rnk_n, n, howmany, block, gc_below, gc_above,
      data, comm_cart, gc_flags);
}

PX(gcplan) PX(plan_cgc)(
    int rnk_n, const INT *n, const INT *gc_below, const INT *gc_above,
    C *data, MPI_Comm comm_cart, unsigned gc_flags
    )
{
  INT howmany = 1, *block = PFFT_DEFAULT_BLOCKS;

  return PX(plan_many_cgc)(
      rnk_n, n, howmany, block, gc_below, gc_above,
      data, comm_cart, gc_flags);
}

void PX(exchange)(
    PX(gcplan) ths
    )
{
  if(ths==NULL)
    return;

  ths->timer_exg->iter++;
  ths->timer_exg->whole -= MPI_Wtime();
  PX(exchange_gc)(ths);
  ths->timer_exg->whole += MPI_Wtime();
}


void PX(reduce)(
    PX(gcplan) ths
    )
{
  if(ths==NULL)
    return;

  ths->timer_red->iter++;
  ths->timer_red->whole -= MPI_Wtime();
  PX(reduce_gc)(ths);
  ths->timer_red->whole += MPI_Wtime();
}



static void print_array(
     const R *data, const INT *n, const INT *start, const char *name, const int is_complex
     )
{
  INT k0, k1, k2, l=0;
  
  if( PX(prod_INT)(3, n) < 1)
    return;

  for(k0 = 0; k0 < n[0]; k0++){
    printf("%s(%td,%td:%td,%td:%td):\n", name, start[0]+k0, start[1],
        start[1]+n[1]-1, start[2], start[2]+n[2]-1);
    for(k1 = 0; k1 < n[1]; k1++){
      printf("  ");
      for(k2 = 0; k2 < n[2]; k2++, l++){
        if( is_complex )
          printf("  %.2e + %.2ei,", (double) data[2*l], (double) data[2*l+1]);
        else
          printf("  %.2e,", (double) data[l]);
      }
      printf("\n");
    }
  }
  printf("\n");
}


static void print_complex_array(
     const R *data, const INT *n, const INT *start, const char *name
     )
{
  print_array(data, n, start, name, 1);
}


static void print_real_array(
     const R *data, const INT *n, const INT *start, const char *name
     )
{
  print_array(data, n, start, name, 0);
}


void PX(apr_complex_3d)(
     const C *data, const INT *local_n, const INT *local_start,
     const char *name, MPI_Comm comm
     )
{
  int num_procs, proc_rank;
  
  MPI_Comm_size(comm, &num_procs);
  MPI_Comm_rank(comm, &proc_rank);

  fflush(stdout);
  for(int t=0; t<num_procs; t++){
    if(proc_rank == t){
      printf("Rank %d:\n", proc_rank);
      print_complex_array((R*) data, local_n, local_start, name);
      fflush(stdout);
    }
    MPI_Barrier(comm);
  }
}


void PX(apr_real_3d)(
     const R *data, const INT *local_n, const INT *local_start,
     const char *name, MPI_Comm comm
     )
{
  int num_procs, proc_rank;
  
  MPI_Comm_size(comm, &num_procs);
  MPI_Comm_rank(comm, &proc_rank);

  fflush(stdout);
  for(int t=0; t<num_procs; t++){
    if(proc_rank == t){
      printf("Rank %d:\n", proc_rank);
      print_real_array( data, local_n, local_start, name);
      fflush(stdout);
    }
    MPI_Barrier(comm);
  }
}


void PX(apr_complex_permuted_3d)(
     const C *data, const INT *local_n, const INT *local_start,
     int perm0, int perm1, int perm2,
     const char *name, MPI_Comm comm
     )
{
  int perm[3];
  INT local_start_perm[3], local_n_perm[3];
  
  perm[0] = perm0; perm[1] = perm1; perm[2] = perm2;
  
  for(int t=0; t<3; t++){
    local_n_perm[t] = local_n[perm[t]];
    local_start_perm[t] = local_start[perm[t]];
  }
  
  PX(apr_complex_3d)(data, local_n_perm, local_start_perm, name, comm);
}














/* 3d interface */
INT PX(local_size_dft_3d)(
    const INT *n,
    MPI_Comm comm_cart, unsigned pfft_flags,
    INT *local_ni, INT *local_i_start,
    INT *local_no, INT *local_o_start
    )
{
  int rnk_n = 3;

  return PX(local_size_dft)(rnk_n, n, comm_cart, pfft_flags,
      local_ni, local_i_start, local_no, local_o_start);
}

INT PX(local_size_dft_r2c_3d)(
    const INT *n,
    MPI_Comm comm_cart, unsigned pfft_flags,
    INT *local_ni, INT *local_i_start,
    INT *local_no, INT *local_o_start
    )
{
  int rnk_n = 3;

  return PX(local_size_dft_r2c)(rnk_n, n, comm_cart, pfft_flags,
      local_ni, local_i_start, local_no, local_o_start);
}

INT PX(local_size_dft_c2r_3d)(
    const INT *n,
    MPI_Comm comm_cart, unsigned pfft_flags,
    INT *local_ni, INT *local_i_start,
    INT *local_no, INT *local_o_start
    )
{
  int rnk_n = 3;

  return PX(local_size_dft_c2r)(rnk_n, n, comm_cart, pfft_flags,
      local_ni, local_i_start, local_no, local_o_start);
}

INT PX(local_size_r2r_3d)(
    const INT *n,
    MPI_Comm comm_cart, unsigned pfft_flags,
    INT *local_ni, INT *local_i_start,
    INT *local_no, INT *local_o_start
    )
{
  int rnk_n = 3;

  return PX(local_size_r2r)(rnk_n, n, comm_cart, pfft_flags,
      local_ni, local_i_start, local_no, local_o_start);
}

PX(plan) PX(plan_dft_3d)(
    const INT *n, C *in, C *out, MPI_Comm comm_cart,
    int sign, unsigned pfft_flags
    )
{
  int rnk_n = 3;

  return PX(plan_dft)(rnk_n, n, in, out,
      comm_cart, sign, pfft_flags);
}

PX(plan) PX(plan_dft_r2c_3d)(
    const INT *n, R *in, C *out, MPI_Comm comm_cart,
    int sign, unsigned pfft_flags
    )
{
  int rnk_n = 3;
  
  return PX(plan_dft_r2c)(rnk_n, n, in, out,
      comm_cart, sign, pfft_flags);
}

PX(plan) PX(plan_dft_c2r_3d)(
    const INT *n, C *in, R *out, MPI_Comm comm_cart,
    int sign, unsigned pfft_flags
    )
{
  int rnk_n = 3;
  
  return PX(plan_dft_c2r)(rnk_n, n, in, out,
      comm_cart, sign, pfft_flags);
}

PX(plan) PX(plan_r2r_3d)(
    const INT *n, R *in, R *out, MPI_Comm comm_cart,
    const PX(r2r_kind) *kinds, unsigned pfft_flags
    )
{
  int rnk_n = 3;

  return PX(plan_r2r)(rnk_n, n, in, out,
      comm_cart, kinds, pfft_flags);
}


/* basic interface */
INT PX(local_size_dft)(
    int rnk_n, const INT *n,
    MPI_Comm comm_cart, unsigned pfft_flags,
    INT *local_ni, INT *local_i_start,
    INT *local_no, INT *local_o_start
    )
{
  INT howmany = 1;

  return PX(local_size_many_dft)(rnk_n, n, n, n, howmany,
      PFFT_DEFAULT_BLOCKS, PFFT_DEFAULT_BLOCKS, comm_cart, pfft_flags,
      local_ni, local_i_start, local_no, local_o_start);
}

INT PX(local_size_dft_r2c)(
    int rnk_n, const INT *n,
    MPI_Comm comm_cart, unsigned pfft_flags,
    INT *local_ni, INT *local_i_start,
    INT *local_no, INT *local_o_start
    )
{
  INT howmany = 1;

  return PX(local_size_many_dft_r2c)(rnk_n, n, n, n, howmany,
      PFFT_DEFAULT_BLOCKS, PFFT_DEFAULT_BLOCKS, comm_cart, pfft_flags,
      local_ni, local_i_start, local_no, local_o_start);
}

INT PX(local_size_dft_c2r)(
    int rnk_n, const INT *n,
    MPI_Comm comm_cart, unsigned pfft_flags,
    INT *local_ni, INT *local_i_start,
    INT *local_no, INT *local_o_start
    )
{
  INT howmany = 1;

  return PX(local_size_many_dft_c2r)(rnk_n, n, n, n, howmany,
      PFFT_DEFAULT_BLOCKS, PFFT_DEFAULT_BLOCKS, comm_cart, pfft_flags,
      local_ni, local_i_start, local_no, local_o_start);
}

INT PX(local_size_r2r)(
    int rnk_n, const INT *n,
    MPI_Comm comm_cart, unsigned pfft_flags,
    INT *local_ni, INT *local_i_start,
    INT *local_no, INT *local_o_start
    )
{
  INT howmany = 1;

  return PX(local_size_many_r2r)(rnk_n, n, n, n, howmany,
      PFFT_DEFAULT_BLOCKS, PFFT_DEFAULT_BLOCKS, comm_cart, pfft_flags,
      local_ni, local_i_start, local_no, local_o_start);
}



PX(plan) PX(plan_dft)(
    int rnk_n, const INT *n, C *in, C *out, MPI_Comm comm_cart,
    int sign, unsigned pfft_flags
    )
{
  INT howmany = 1;
  
  return PX(plan_many_dft)(
    rnk_n, n, n, n, howmany, PFFT_DEFAULT_BLOCKS, PFFT_DEFAULT_BLOCKS,
    in, out, comm_cart, sign, pfft_flags);
}

PX(plan) PX(plan_dft_r2c)(
    int rnk_n, const INT *n, R *in, C *out, MPI_Comm comm_cart,
    int sign, unsigned pfft_flags
    )
{
  INT howmany = 1;
  
  return PX(plan_many_dft_r2c)(
    rnk_n, n, n, n, howmany, PFFT_DEFAULT_BLOCKS, PFFT_DEFAULT_BLOCKS,
    in, out, comm_cart, sign, pfft_flags);
}

PX(plan) PX(plan_dft_c2r)(
    int rnk_n, const INT *n, C *in, R *out, MPI_Comm comm_cart,
    int sign, unsigned pfft_flags
    )
{
  INT howmany = 1;
  
  return PX(plan_many_dft_c2r)(
    rnk_n, n, n, n, howmany, PFFT_DEFAULT_BLOCKS, PFFT_DEFAULT_BLOCKS,
    in, out, comm_cart, sign, pfft_flags);
}

PX(plan) PX(plan_r2r)(
    int rnk_n, const INT *n, R *in, R *out, MPI_Comm comm_cart,
    const PX(r2r_kind) *kinds, unsigned pfft_flags
    )
{
  INT howmany = 1;
  
  return PX(plan_many_r2r)(
    rnk_n, n, n, n, howmany, PFFT_DEFAULT_BLOCKS, PFFT_DEFAULT_BLOCKS,
    in, out, comm_cart, kinds, pfft_flags);
}



/* functions to execute and destroy PX(plan) */

void PX(execute)(
    PX(plan) ths
    )
{
  int r;
  
  if(ths==NULL){
    PX(fprintf)(MPI_COMM_WORLD, stderr,
        "!!! Error: Can not execute PFFT Plan == NULL !!!\n");
    return;
  }

  /* set global variables for easy debbuging  */
#if PFFT_DEBUG_GVARS
  gdbg_comm_cart = &ths->comm_cart;
  gdbg_rnk_pm = ths->rnk_pm;
  gdbg_comms_pm = ths->comms_pm;
#endif

  r = ths->rnk_pm;

/*  for (INT d=0; d < ths->rnk_n; d++)
  PX(fprintf)(MPI_COMM_WORLD, stderr,
      "d: %d, n: %d, ni: %d, no: %d, rnk_n: %d, rnk_pm: %d, lb: %d\n", d, ths->n[d], ths->ni[d], ths->no[d], ths->rnk_n, ths->rnk_pm, (d < ths->rnk_pm) ? PX(local_block_size_shifted)(ths->n[d], ths->mblock[d], 0, ths->comms_pm[d]) : 0);
      */

  if ((ths->trafo_flag & PFFTI_TRAFO_C2R) && (ths->sign == PFFT_FORWARD))
    complex_conjugate(ths->in, ths->rnk_n, ths->rnk_pm, ths->ni, ths->comms_pm, ths->iblock);

  ths->timer->whole -= MPI_Wtime();

  /* twiddle inputs in order to get outputs shifted by n/2 */
  ths->timer->itwiddle -= MPI_Wtime(); 
  if(ths->pfft_flags & PFFT_SHIFTED_OUT)
    twiddle_input(ths);
  ths->timer->itwiddle += MPI_Wtime(); 

  ths->timer->remap_3dto2d[0] -= MPI_Wtime(); 
  PX(execute_remap_3dto2d)(ths->remap_3dto2d[0]);
  ths->timer->remap_3dto2d[0] += MPI_Wtime(); 

  execute_transposed(r, ths->serial_trafo, ths->global_remap,
      ths->timer->trafo, ths->timer->remap);

  execute_transposed(r, &ths->serial_trafo[r+1], &ths->global_remap[r],
      &ths->timer->trafo[r+1], &ths->timer->remap[r]);
  
  ths->timer->remap_3dto2d[1] -= MPI_Wtime(); 
  PX(execute_remap_3dto2d)(ths->remap_3dto2d[1]);
  ths->timer->remap_3dto2d[1] += MPI_Wtime(); 

  /* twiddle outputs in order to get inputs shifted by n/2 */
  ths->timer->otwiddle -= MPI_Wtime(); 
  if(ths->pfft_flags & PFFT_SHIFTED_IN)
    twiddle_output(ths);
  ths->timer->otwiddle += MPI_Wtime();

  if ((ths->trafo_flag & PFFTI_TRAFO_R2C) && (ths->sign == PFFT_BACKWARD))
    complex_conjugate(ths->out, ths->rnk_n, ths->rnk_pm, ths->no, ths->comms_pm, ths->oblock);

  ths->timer->iter++;
  ths->timer->whole += MPI_Wtime();
}


static int* malloc_and_transpose_int(
    int rnk_n, int rnk_pm, int transp,
    int *n
    )
{
  int *nt = PX(malloc_int)(rnk_n);

  for(int t=0; t<rnk_pm; t++)
    nt[t] = transp ? n[t+1] : n[t];
  
  nt[rnk_pm] = transp ? n[0] : n[rnk_pm];

  for(int t=rnk_pm+1; t<rnk_n; t++)
    nt[t] = n[t];

  return nt;
} 

static INT* malloc_and_transpose_INT(
    int rnk_n, int rnk_pm, int transp,
    INT *n
    )
{
  INT *nt = PX(malloc_INT)(rnk_n);

  for(int t=0; t<rnk_pm; t++)
    nt[t] = transp ? n[t+1] : n[t];
  
  nt[rnk_pm] = transp ? n[0] : n[rnk_pm];

  for(int t=rnk_pm+1; t<rnk_n; t++)
    nt[t] = n[t];

  return nt;
} 

/* twiddle inputs in order to get outputs shifted by n/2 */
static void twiddle_input(
    PX(plan) ths
    )
{
  if(ths->itwiddle_in == NULL)
    return;

  INT howmany = ths->howmany, l;
  R factor;
  INT *n = malloc_and_transpose_INT(ths->rnk_n, ths->rnk_pm, ths->transp_flag & PFFT_TRANSPOSED_IN, ths->n);
  INT *local_ni = malloc_and_transpose_INT(ths->rnk_n, ths->rnk_pm, ths->transp_flag & PFFT_TRANSPOSED_IN, ths->local_ni);
  INT *local_ni_start = malloc_and_transpose_INT(ths->rnk_n, ths->rnk_pm, ths->transp_flag & PFFT_TRANSPOSED_IN, ths->local_ni_start);
  int *skip_trafos = malloc_and_transpose_int(ths->rnk_n, ths->rnk_pm, ths->transp_flag & PFFT_TRANSPOSED_IN, ths->skip_trafos);

  if( (ths->trafo_flag & PFFTI_TRAFO_C2C) || (ths->trafo_flag & PFFTI_TRAFO_C2R ) )
    howmany *= 2;
  
  INT local_n_total = PX(prod_INT)(ths->rnk_n, local_ni);

  for(INT k=0; k<local_n_total; k++){
    l = k;
    factor = 1.0;
    for(int t=ths->rnk_n-1; t>=0; t--){
      INT kt = l%local_ni[t];
      if(!skip_trafos[t])
        factor *= (kt + local_ni_start[t] + n[t]/2) % 2 ? -1.0 : 1.0;
      l /= local_ni[t];
    }

    for(INT h=0; h<howmany; h++)
      ths->itwiddle_out[howmany*k+h] = ths->itwiddle_in[howmany*k+h] * factor;
//     fprintf(stderr, "pfft: api-basic: in[%2td] = %.2e + I* %.2e\n", k, ths->in[howmany*k], ths->in[howmany*k+1]);
  }

  free(n); free(local_ni); free(local_ni_start); free(skip_trafos);
}


/* twiddle outputs in order to get inputs shifted by n/2 */
static void twiddle_output(
    PX(plan) ths
    )
{
  if(ths->otwiddle_in == NULL)
    return;

  INT howmany = ths->howmany, l;
  R factor;
  INT *local_no = malloc_and_transpose_INT(ths->rnk_n, ths->rnk_pm, ths->transp_flag & PFFT_TRANSPOSED_OUT, ths->local_no);
  INT *local_no_start = malloc_and_transpose_INT(ths->rnk_n, ths->rnk_pm, ths->transp_flag & PFFT_TRANSPOSED_OUT, ths->local_no_start);
  int *skip_trafos = malloc_and_transpose_int(ths->rnk_n, ths->rnk_pm, ths->transp_flag & PFFT_TRANSPOSED_OUT, ths->skip_trafos);
  
  if( (ths->trafo_flag & PFFTI_TRAFO_C2C) || (ths->trafo_flag & PFFTI_TRAFO_R2C ) )
    howmany *= 2;

  INT local_n_total = PX(prod_INT)(ths->rnk_n, local_no);

  for(INT k=0; k<local_n_total; k++){
    l = k;
    factor = 1.0;
    for(int t=ths->rnk_n-1; t>=0; t--){
      INT kt = l%local_no[t];
      if(!skip_trafos[t])
        factor *= (kt + local_no_start[t]) % 2 ? -1.0 : 1.0;
      l /= local_no[t];
    }

    for(INT h=0; h<howmany; h++)
      ths->otwiddle_out[howmany*k+h] = ths->otwiddle_in[howmany*k+h] * factor;
//     fprintf(stderr, "pfft: api-basic: out[%2td] = %.2e + I* %.2e\n", k, ths->out[howmany*k], ths->out[howmany*k+1]);
  }

  free(local_no); free(local_no_start); free(skip_trafos);
}


static void execute_transposed(
    int rnk_pm, outrafo_plan *trafos, gtransp_plan *remaps,
    double *timer_trafo, double *timer_remap
    )
{
  int t;
  
  for(t=0; t<rnk_pm; t++){
    timer_trafo[t] -= MPI_Wtime();
    PX(execute_outrafo)(trafos[t]);
    timer_trafo[t] += MPI_Wtime();

    timer_remap[t] -= MPI_Wtime();
    PX(execute_gtransp)(remaps[t]);
    timer_remap[t] += MPI_Wtime();
  }
  
  timer_trafo[t] -= MPI_Wtime();
  PX(execute_outrafo)(trafos[t]);
  timer_trafo[t] += MPI_Wtime();
}



