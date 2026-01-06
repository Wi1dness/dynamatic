/*
 * This code implements the Matricized Tensor Times Khatri-Rao Product (MTTKRP), which performs:
 * D(i,j) += A(i,k,l) * B(k,j) * C(l,j)
 * Input: A[I][K][L], B[K][J], C[L][J]
 * Output: D[I][J]
 */

#include "mttkrp.h"
#include "dynamatic/Integration.h"

void mttkrp(data_t A[I][K][L], data_t B[K][J], data_t C[J][L], data_t D[I][J]) {
  for (int i = 0; i < I; i++)
    for (int j = 0; j < J; j++) {
      D[i][j] = 0;
      for (int k = 0; k < K; k++) {
        for (int l = 0; l < L; l++) {
//          D[i][j] += A[i][k][l] * B[k][j] * C[l][j];
          D[i][j] = D[i][j] + A[i][k][l] * B[k][j] * C[j][l];
        }
      }
    }
}

int main(int argc, char **argv){
  // declarations
  static data_t A[I][K][L];
  static data_t B[K][J];
//  static data_t C[L][J];
  static data_t C[J][L];
  static data_t D[I][J];
  static data_t D_golden[I][J];

  // data initialization
  for (int i = 0; i < I; i++)
    for (int k = 0; k < K; k++) 
      for (int l = 0; l < L; l++) {
        A[i][k][l] = 2.5;
      }
  for (int k = 0; k < K; k++)
    for (int j = 0; j < J; j++) {
      B[k][j] = 2.5;
    }
  for (int l = 0; l < L; l++)
    for (int j = 0; j < J; j++) {
//      C[l][j] = 2.5;
      C[j][l] = 2.5;
    }
  data_t tmp;

  // computation
  CALL_KERNEL(mttkrp, A, B, C, D);

  return 0;
}