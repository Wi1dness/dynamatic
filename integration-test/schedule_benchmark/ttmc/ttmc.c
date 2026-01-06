/*
 * This code implements the Chain of Tensor-matrix multiplications (TTMc), which performs:
 * D(i,j,k) += A(i,l,m) * B(l,j) * C(m,k)
 * Input: A[I][L][M], B[L][J], C[M][K]
 * Output: D[I][J][K]
 */

#include "ttmc.h"
#include "dynamatic/Integration.h"

void ttmc(data_t A[I][L][M], data_t B[L][J], data_t C[K][M], data_t D[I][J][K]) {
  for (int i = 0; i < I; i++)
    for (int j = 0; j < J; j++) 
      for (int k = 0; k < K; k++) {
        D[i][j][k] = 0;        
        for (int l = 0; l < L; l++) 
          for (int m = 0; m < M; m++) {
//            D[i][j][k] = D[i][j][k] + A[i][l][m] * B[l][j] * C[m][k];
            D[i][j][k] = D[i][j][k] + A[i][l][m] * B[l][j] * C[k][m];
          }
      }    
}

int main(int argc, char **argv){
  // declarations
  static data_t A[I][L][M];
  static data_t B[L][J];
//  static data_t C[M][K];
  static data_t C[K][M];
  static data_t D[I][J][K];
  static data_t D_golden[I][J][K];

  // data initialization
  for (int i = 0; i < I; i++)
    for (int l = 0; l < L; l++) 
      for (int m = 0; m < M; m++) {
        A[i][l][m] = 2.5;
      }
  for (int l = 0; l < L; l++)
    for (int j = 0; j < J; j++) {
      B[l][j] = 2.5;
    }
  for (int m = 0; m < M; m++)
    for (int k = 0; k < K; k++) {
//      C[m][k] = 2.5;
      C[k][m] = 2.5;
    }
  
  // computation
  CALL_KERNEL(ttmc, A, B, C, D);

  return 0;
}