/* { dg-do run { target *-*-linux* } } */
/* { dg-additional-sources "../sync-1.c" } */
/* { dg-options "-Dxchg -Dtype=int -Dmisalignment=3" } */
#include "sync-mis-op-s-1.c"