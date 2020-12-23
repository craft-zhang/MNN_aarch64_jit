/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Dhiraj Kalamkar (Intel Corp.)
******************************************************************************/

#include <vector>
#include <time.h>
#include <sys/syscall.h>
#include <algorithm>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <omp.h>
#include "utils.h"
#include "EmbeddingBag.h"
#include "dist.h"

#ifdef RTM_DEBUG
int rtm_stats[1000][16];
#endif

//#define USE_RTM

int my_rank = 0;
int my_size = 1;

struct EmbeddingInOut {
  int N, NS, E;
  ITyp *offsets;
  ITyp *indices;
  FTyp *output;
  FTyp *gradout;
  FTyp *grads;
};

void allocate_buffers_and_generte_rnd_input(int N, int P, EmbeddingBag *eb, EmbeddingInOut *eio)
{
  int E = eb->E;
  int M = eb->M;
  int NS = 0;
  eio->N = N;
  eio->E = E;

  eio->offsets = (ITyp*)my_malloc((N+1) * sizeof(ITyp), alignment);
  eio->output = (FTyp*)my_malloc(N * E * sizeof(FTyp), alignment);
  eio->gradout = (FTyp*)my_malloc(N * E * sizeof(FTyp), alignment);

  eio-> offsets[0] = 0;
  for(int i = 1; i <= N; i++) {
    double randval;
    drand48_r(&rand_buf, &randval);
    int cp = (int)(randval * P);
    if (cp == 0) cp = 1;
    NS += cp;
    eio->offsets[i] = NS;
  }
  eio->NS = NS;
  eio->indices = (ITyp*)my_malloc(NS * sizeof(ITyp), alignment);
  eio->grads = (FTyp*)my_malloc(NS * E * sizeof(FTyp), alignment);
  for (int n = 0; n < N; n++)
  {
    int start = eio->offsets[n];
    int end = eio->offsets[n+1];
    for (int i = start; i < end; i++)
    {
      double randval;
      drand48_r(&rand_buf, &randval);
      ITyp ind = (ITyp)(randval * M);
      if (ind == M)
        ind--;
      eio->indices[i] = ind;
    }
    std::sort(&eio->indices[start], &eio->indices[end]);
  }
}

void free_buffers(EmbeddingInOut *eio)
{
  my_free(eio->grads);
  my_free(eio->indices);
  my_free(eio->gradout);
  my_free(eio->output);
  my_free(eio->offsets);
}

void pack_for_a2a(int LS, int N, int E, EmbeddingInOut **emb, FTyp *a2aSrc)
{
  for (int i = 0; i < LS; i++)
  {
    for (int n = 0; n < N; n++)
    {
      for (int v = 0; v < E; v++)
      {
        a2aSrc[n * LS * E + i * E + v] = emb[i]->output[n * E + v];
      }
    }
  }
}

void unpack_from_a2a(int LS, int N, int E, EmbeddingInOut **emb, FTyp *a2aGDst)
{
  for (int i = 0; i < LS; i++)
  {
    for (int n = 0; n < N; n++)
    {
      for (int v = 0; v < E; v++)
      {
        emb[i]->gradout[n * E + v] = a2aGDst[n * LS * E + i * E + v];
      }
    }
  }
}

void alltoall(size_t size, FTyp *src, FTyp *dst)
{
  if (my_size <= 1)
    return;
  size_t sz = size / my_size;
  if (sizeof(FTyp) == 4)
  {
    dist_alltoall(sz, src, dst);
  }
  else
  {
    printf("Datatype not supported\n");
    exit(1);
  }
}

double get_checksum(FTyp *buf, size_t sz)
{
  double sum = 0.0;
#pragma omp parallel for reduction(+:sum)
  for(size_t i = 0; i < sz; i++) {
    sum += buf[i];
  }
  return sum;
}

int iters = 100;
int N = 2048;
int E = 64;
int P = 100;
int M = 1000000;
int S = 8;

#define my_printf(fmt, args...) printf("[%d] " fmt, my_rank, args)

int main(int argc, char * argv[]) {
  int NP = 1;
  dist_init(&argc, &argv);
  my_rank = dist_get_rank();
  my_size = dist_get_size();

  if(argc > 1 && strncmp(argv[1], "-h", 3) == 0) {
    printf("Usage: %s iters N E M S P\n", argv[0]);
    printf("iters: Number of iterations (= %d)\n", iters);
    printf("N: Minibatch (= %d)\n", N);
    printf("E: embedding row width (= %d)\n", E);
    printf("M: Number of rows per table (= %d)\n", M);
    printf("S: Number of Tables (= %d)\n", S);
    printf("P: Average number of indices per look up (= %d)\n", P);
    dist_fini();
    exit(0);
  }

  {
    int i = 1;
    if(argc > i) iters = atoi(argv[i++]);
    if(argc > i) N = atoi(argv[i++]);
    if(argc > i) E = atoi(argv[i++]);
    if(argc > i) M = atoi(argv[i++]);
    if(argc > i) S = atoi(argv[i++]);
    if(argc > i) P = atoi(argv[i++]);
  }

  printf("Using: iters: %d N: %d E: %d M: %d S: %d P: %d\n", iters, N, E, M, S, P);


#if defined(USE_RTM) && defined(RTM_DEBUG)
  clear_rtm_stats();
#endif

  double checksum = 0.0;

  int LS = S / my_size;
  int LN = N / my_size;
  set_random_seed(777+my_rank);

  EmbeddingInOut *eio[iters][LS];
  EmbeddingBag *eb[LS];
  FTyp *A2Asrc, *A2Adst;
  FTyp *A2Agsrc, *A2Agdst;
  size_t tNS = 0;

  A2Asrc = (FTyp*)my_malloc(LS*N*E*sizeof(FTyp), alignment);
  A2Agsrc = (FTyp*)my_malloc(S*LN*E*sizeof(FTyp), alignment);

  init_random(S*LN*E, A2Agsrc, -0.01f, 0.01f);

  if (my_size > 1)
  {
    A2Adst = (FTyp *)my_malloc(S * LN * E * sizeof(FTyp), alignment);
    A2Agdst = (FTyp *)my_malloc(LS * N * E * sizeof(FTyp), alignment);
  }
  else
  {
    A2Adst = A2Asrc;
    A2Agdst = A2Agsrc;
  }

  for(int i = 0; i < LS; i++)
  {
    eb[i] = new EmbeddingBag(M, E);
    for(int j = 0; j < iters; j++)
    {
      eio[j][i] = new EmbeddingInOut();
      allocate_buffers_and_generte_rnd_input(N, P, eb[i], eio[j][i]);
      tNS += eio[j][i]->NS;
    }
  }

  double t0 = get_time();
  double fwdTime = 0.0, bwdTime = 0.0, updTime = 0.0;
  double packTime = 0.0, unpackTime = 0.0, fwdA2ATime = 0.0, bwdA2ATime = 0.0;

  for(int i = 0; i < iters; i++) {
    double t0 = get_time();
    for(int s = 0; s < LS; s++) {
      eb[s]->forward(N, eio[i][s]->NS, eio[i][s]->offsets, eio[i][s]->indices, eio[i][s]->output);
    }
    double t1 = get_time();
    pack_for_a2a(LS, N, E, eio[i], A2Asrc);
    double t2 = get_time();
    alltoall(LS*N*E, A2Asrc, A2Adst);
    double t3 = get_time();
    alltoall(LS*N*E, A2Agsrc, A2Agdst);
    double t4 = get_time();
    unpack_from_a2a(LS, N, E, eio[i], A2Agdst);
    double t5 = get_time();

    for(int s = LS-1; s >= 0; s--) {
      eb[s]->backward(N, eio[i][s]->NS, eio[i][s]->gradout, eio[i][s]->offsets, eio[i][s]->indices, eio[i][s]->grads);
    }
    double t6 = get_time();
    for(int s = 0; s < LS; s++) {
      eb[s]->update(eio[i][s]->NS, eio[i][s]->grads, eio[i][s]->indices, -0.1);
    }
    double t7 = get_time();
    //printf("Iter %4d: F = %.3f   B = %.3f   U = %.3f\n", i, t1-t0, t2-t1, t3-t2);
    fwdTime += t1-t0;
    bwdTime += t6-t5;
    updTime += t7-t6;
    packTime += t2-t1;
    unpackTime += t5-t4;
    fwdA2ATime += t3-t2;
    bwdA2ATime += t4-t3;
  }
  double t1 = get_time();
#ifdef VERIFY_CORRECTNESS
  for(int s = 0; s < LS; s++) {
    double psum = get_checksum(&weight[s][0][0], M*E);
    //my_printf("PSUM %d: %g\n", SS+s, psum);
    checksum += psum;
  }
#endif
#ifdef STREAMING_WRITES
  const size_t rfo = 1;
#else
  const size_t rfo = 2;
#endif

  size_t fwdBytes = ((size_t)tNS*E + (size_t)rfo*iters*LS*N*E) * sizeof(FTyp) + ((size_t)tNS + (size_t)iters*LS*N) * sizeof(ITyp);
  int use_rtm = 1;
  size_t bwdBytes = ((size_t)rfo*tNS*E + (size_t)iters*LS*N*E) * sizeof(FTyp) + ((size_t)tNS) * sizeof(ITyp);
  size_t updBytes = ((size_t)3*tNS*E) * sizeof(FTyp) + ((size_t)tNS) * sizeof(ITyp);

  my_printf("USE RTM = %d  STREAMING STORES = %d\n", use_rtm, rfo == 1 ? 1 : 0);
  my_printf("Iters = %d, LS = %d, N = %d, M = %d, E = %d, avgNS = %d, P = %d\n", iters, LS, N, M, E, tNS/(iters*LS), P);
  //printf("Time: Fwd: %.3f ms Bwd: %.3f ms Upd: %.3f  Total: %.3f\n", fwdTime, bwdTime, updTime, t1-t0);
  my_printf("Per Iter  Time: Fwd: %.3f ms Bwd: %.3f ms Upd: %.3f  A2A: %.3f ms Total: %.3f ms\n", fwdTime/(iters), bwdTime/(iters), updTime/(iters), (fwdA2ATime+bwdA2ATime+packTime+unpackTime)/(iters), (t1-t0)/(iters));
  my_printf("Per Table Time: Fwd: %.3f ms Bwd: %.3f ms Upd: %.3f  Total: %.3f ms\n", fwdTime/(iters*LS), bwdTime/(iters*LS), updTime/(iters*LS), (t1-t0)/(iters*LS));

  my_printf("Per Iter  A2ATime: Fwd: %.3f ms Bwd: %.3f ms Pack: %.3f ms Unpack: %.3f ms \n", fwdA2ATime/(iters), bwdA2ATime/(iters), packTime/(iters), unpackTime/(iters));
  my_printf("BW: FWD: %.3f   BWD: %.3f GB/s   UPD: %.3f GB/s\n", fwdBytes*1e-6/fwdTime, bwdBytes*1e-6/bwdTime, updBytes*1e-6/updTime);


#ifdef VERIFY_CORRECTNESS
  printf("Checksum = %g\n", checksum);
#endif

#if defined(USE_RTM) && defined(RTM_DEBUG)
  print_rtm_stats();
#endif

  for(int i = 0; i < LS; i++)
  {
    for(int j = 0; j < iters; j++)
    {
      free_buffers(eio[j][i]);
      delete eio[j][i];
    }
    delete eb[i];
  }
  if (my_size > 1)
  {
    my_free(A2Agdst);
    my_free(A2Adst);
  }

  my_free(A2Agsrc);
  my_free(A2Asrc);
  dist_fini();
  return 0;
}
