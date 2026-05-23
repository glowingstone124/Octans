// Minimal 64-bit integer division helpers for freestanding Lamp kernel builds.
// These provide the compiler-rt/libgcc symbols Clang may emit on 32-bit targets.

typedef unsigned long long du_int;
typedef long long di_int;

static du_int uabsdi(di_int V) {
  du_int UV = (du_int)V;
  return (V < 0) ? (du_int)(0 - UV) : UV;
}

du_int __udivmoddi4(du_int A, du_int B, du_int *Rem) {
  if (B == 0) {
    if (Rem)
      *Rem = 0;
    return 0;
  }

  if (A < B) {
    if (Rem)
      *Rem = A;
    return 0;
  }

  du_int Div = B;
  unsigned Shift = 0;
  while ((Div >> 63) == 0 && Div <= (A >> 1)) {
    Div <<= 1;
    ++Shift;
  }

  du_int Quot = 0;
  for (;;) {
    Quot <<= 1;
    if (A >= Div) {
      A -= Div;
      Quot |= 1;
    }
    if (Shift == 0)
      break;
    Div >>= 1;
    --Shift;
  }

  if (Rem)
    *Rem = A;
  return Quot;
}

du_int __udivdi3(du_int A, du_int B) { return __udivmoddi4(A, B, (du_int *)0); }

du_int __umoddi3(du_int A, du_int B) {
  du_int R;
  (void)__udivmoddi4(A, B, &R);
  return R;
}

di_int __divdi3(di_int A, di_int B) {
  if (B == 0)
    return 0;

  int Neg = ((A < 0) ^ (B < 0)) != 0;
  du_int UA = uabsdi(A);
  du_int UB = uabsdi(B);
  du_int UQ = __udivdi3(UA, UB);
  di_int Q = (di_int)UQ;
  return Neg ? -Q : Q;
}

di_int __moddi3(di_int A, di_int B) {
  if (B == 0)
    return 0;

  du_int UA = uabsdi(A);
  du_int UB = uabsdi(B);
  du_int UR = __umoddi3(UA, UB);
  di_int R = (di_int)UR;
  return (A < 0) ? -R : R;
}
