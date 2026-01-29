#include "loop_store.h"
#include "dynamatic/Integration.h"

// void loop_store(inout_int_t a[N]) {
//   for (unsigned i = 0; i < N; ++i) {
//     unsigned x = i;
//     if (a[i] == 0)
//       x = x * x;
//     a[i] = x;
//   }
// }

void loop_store(inout_int_t a[N], in_int_t b[N]) {
  for (unsigned i = 0; i < N; ++i) {
    unsigned x = i;
    if (a[i] == 0)
      x = b[i] * x;
    a[i] = x;
  }
}

int main(void) {
  inout_int_t a[N];
  in_int_t b[N];
  // Initialize a to [0, 1, 0, 1, ...]
  for (unsigned i = 0; i < N; ++i)
    a[i] = i % 2;
  CALL_KERNEL(loop_store, a, b);
  return 0;
}
