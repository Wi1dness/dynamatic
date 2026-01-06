/*
 * This code implements the Tensor Times Matrix (TTM), which performs:
 * C(i,j,k) += A(i,j,l) * B(l,k)
 * Input: A[I][J][L], B[L][K]
 * Output: C[I][J][K]
 */

#include "ttm.h"
#include "dynamatic/Integration.h"

void ttm(data_t A[I][J][L], data_t B[K][L], data_t C[I][J][K]) {
  for (int i = 0; i < I; i++)
    for (int j = 0; j < J; j++) 
      for (int k = 0; k < K; k++) {
//        C[i][j][k] = 0;
        for (int l = 0; l < L; l++) {
//          C[i][j][k] = C[i][j][k] + A[i][j][l] * B[l][k];
          C[i][j][k] = C[i][j][k] + A[i][j][l] * B[k][l];
        }
      }
}

int main(int argc, char **argv){
  // declarations
  static data_t A[I][J][L];
//  static data_t B[L][K];
  static data_t B[K][L];
  static data_t C[I][J][K];
  static data_t C_golden[I][J][K];

  // data initialization
  for (int i = 0; i < I; i++)
    for (int j = 0; j < J; j++) 
      for (int l = 0; l < L; l++) {
        A[i][j][l] = 2.5;
      }
  for (int l = 0; l < L; l++)
    for (int k = 0; k < K; k++) {
//      B[l][k] = 2.5;
      B[k][l] = 2.5;
    }

  // computation
  CALL_KERNEL(ttm, A, B, C);

  return 0;
}