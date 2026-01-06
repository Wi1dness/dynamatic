// In this example, we compile three different operators that are found often in 
// DNNs, including: point-wise conv, depth-wise conv, and FC.

#include "dnn.h"
#include "dynamatic/Integration.h"

#ifdef PC
void dnn(data_t pc_cin[PC_R + PC_K - 1][PC_C + PC_K - 1][PC_I], data_t pc_w[PC_O][PC_K][PC_K][PC_I], data_t pc_cout[PC_R][PC_C][PC_O]) {
  for (int o = 0; o < PC_O; o++)
    for (int r = 0; r < PC_R; r++)
      for (int c = 0; c < PC_C; c++) {
        pc_cout[r][c][o] = 0;
        for (int i = 0; i < PC_I; i++)
          for (int p = 0; p < PC_K; p++)
            for (int q = 0; q < PC_K; q++) {
              pc_cout[r][c][o] = pc_cout[r][c][o] + pc_cin[r + p][c + q][i] * pc_w[o][p][q][i];
            }
      }
}
#endif

#ifdef DC
void dnn(data_t dc_cin[DC_R + DC_K - 1][DC_C + DC_K - 1][DC_I], data_t dc_w[DC_K][DC_K][DC_I], data_t dc_cout[DC_R][DC_C][DC_O]) {
  for (int o = 0; o < DC_O; o++)
    for (int r = 0; r < DC_R; r++)
      for (int c = 0; c < DC_C; c++) {
        dc_cout[r][c][o] = 0;
        for (int p = 0; p < DC_K; p++)
          for (int q = 0; q < DC_K; q++) {
            dc_cout[r][c][o] = dc_cout[r][c][o] + dc_cin[r + p][c + q][o] * dc_w[p][q][o];
          }
      }
}
#endif

#ifdef FC
void dnn(data_t fc_cin[FC_I][FC_J], data_t fc_w[FC_J], data_t fc_cout[FC_I]) {
  for (int i = 0; i < FC_I; i++) {
    fc_cout[i] = 0;
    for (int j = 0; j < FC_J; j++) {
      fc_cout[i] = fc_cout[i] + fc_cin[i][j] * fc_w[j];
    }
  }
}
#endif

int main(int argc, char **argv){
#ifdef PC	
  // Point-wise CONV
  data_t pc_cin[PC_R + PC_K - 1][PC_C + PC_K - 1][PC_I];
  data_t pc_w[PC_O][PC_K][PC_K][PC_I];
  data_t pc_cout[PC_R][PC_C][PC_O];

  for (int i = 0; i < PC_I; i++)
    for (int r = 0; r < PC_R + PC_K - 1; r++)
      for (int c = 0; c < PC_C + PC_K - 1; c++) {
        pc_cin[r][c][i] = i;
      }

	for (int o = 0; o < PC_O; o++)
		for (int i = 0; i < PC_I; i++)
			for (int p = 0; p < PC_K; p++)
				for (int q = 0; q < PC_K; q++) {
					pc_w[o][p][q][i] = o;
				}

  CALL_KERNEL(dnn, pc_cin, pc_w, pc_cout);
#endif

#ifdef DC
  // Depth-wise CONV
  data_t dc_cin[DC_R + DC_K - 1][DC_C + DC_K - 1][DC_I];
  data_t dc_w[DC_K][DC_K][DC_I];
  data_t dc_cout[DC_R][DC_C][DC_O];

  for (int i = 0; i < DC_I; i++)
    for (int r = 0; r < DC_R + DC_K - 1; r++)
      for (int c = 0; c < DC_C + DC_K - 1; c++) {
        dc_cin[r][c][i] = i;
      }
	
	for (int i = 0; i < DC_I; i++)
		for (int p = 0; p < DC_K; p++)
			for (int q = 0; q < DC_K; q++) {
				dc_w[p][q][i] = i;
			}

  CALL_KERNEL(dnn, dc_cin, dc_w, dc_cout);
#endif

#ifdef FC
  // Fully-connected Layers
  data_t fc_cin[FC_I][FC_J];
  data_t fc_w[FC_J];
  data_t fc_cout[FC_I];

  for (int i = 0; i < FC_I; i++)
    for (int j = 0; j < FC_J; j++) {
      fc_cin[i][j] = i;
    }
	
	for (int j = 0; j < FC_J; j++) {
		fc_w[j] = j;
	}

  CALL_KERNEL(dnn, fc_cin, fc_w, fc_cout);
#endif

  return 0;
}