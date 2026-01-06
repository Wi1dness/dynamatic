#include "cnn.h"
#include "dynamatic/Integration.h"

void cnn(data_t cin[R + K - 1][C + K - 1][I], data_t w[O][K][K][I], data_t cout[R][C][O]) {
  for (int o = 0; o < O; o++)
  for (int r = 0; r < R; r++)
    for (int c = 0; c < C; c++) {
      cout[r][c][o] = 0;
      for (int i = 0; i < I; i++)
        for (int p = 0; p < 3; p++)
          for (int q = 0; q < 3; q++) {
            cout[r][c][o] = cout[r][c][o] + cin[r + p][c + q][i] * w[o][p][q][i];
          }
    }
}

int main(int argc, char **argv){
  // declarations
//  data_t cin[I][R + K - 1][C + K - 1];
//  data_t w[O][I][K][K];
//  data_t cout[O][R][C];
//  data_t cout_golden[O][R][C];
  static data_t cin[R + K - 1][C + K - 1][I];
  static data_t w[O][K][K][I];
  static data_t cout[R][C][O];

  // data initialization
  for (int i = 0 ; i < I; i++)
    for (int r = 0; r < R + K - 1; r++)
      for (int c = 0; c < C + K - 1; c++) {
        cin[r][c][i] = (data_t)rand() / RAND_MAX;
      }

  for (int o = 0; o < O; o++)
    for (int i = 0; i < I; i++) 
      for (int p = 0; p < K; p++)
        for (int q = 0; q < K; q++) {
          w[o][p][q][i] = (data_t)rand() / RAND_MAX;
        }

  for (int r = 0; r < R; r++)
    for (int c = 0; c < C; c++)
      for (int o = 0; o < O; o++) {
        cout[r][c][o] = 0;
      }

  CALL_KERNEL(cnn, cin, w, cout);

  return 0;
}