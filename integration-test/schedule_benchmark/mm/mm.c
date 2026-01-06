#include "mm.h"
#include "dynamatic/Integration.h"

//#define LAYOUT1
#define LAYOUT2
//#define LAYOUT3

void mm(data_t A[I][K], data_t B[J][K], data_t C[I][J]) {
  for (int i = 0; i < I; i++)
    for (int j = 0; j < J; j++) {
      C[i][j] = 0;
      for (int k = 0; k < K; k++) {
#ifdef LAYOUT2        
        C[i][j] = C[i][j] + A[i][k] * B[j][k];
#endif
#ifdef LAYOUT3      
        C[i][j] = C[i][j] + A[k][i] * B[k][j];
#endif        
      }
    }
}

int main(int argc, char **argv) {
//  data_t A[I][K], B[K][J], C[I][J], C_golden[I][J]; 
#ifdef LAYOUT2  
  static data_t A[I][K], B[J][K], C[I][J], C_golden[I][J]; // gemm0,3
#endif  
#ifdef LAYOUT3  
  static data_t A[K][I], B[K][J], C[I][J], C_golden[I][J]; // gemm4
#endif  

  for (int i = 0; i < I; i++) 
    for (int k = 0; k < K; k++) {
#ifdef LAYOUT2      
      A[i][k] = (data_t)rand() / RAND_MAX;
#endif
#ifdef LAYOUT3      
      A[k][i] = (data_t)rand() / RAND_MAX;
#endif      
    }

  for (int j = 0; j < J; j++)
    for (int k = 0; k < K; k++) {
#ifdef LAYOUT2      
      B[j][k] = (data_t)rand() / RAND_MAX;
#endif
#ifdef LAYOUT3      
      B[k][j] = (data_t)rand() / RAND_MAX;
#endif      
    }

  CALL_KERNEL(mm, A, B, C);

  return 0;
}