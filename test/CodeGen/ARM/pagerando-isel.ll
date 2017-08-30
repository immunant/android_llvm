; RUN: llc < %s -mtriple=armv7-linux -relocation-model=pip -o /dev/null 2>&1 \
; RUN:     -print-before=pagerando-optimizer-arm | FileCheck %s

@global_var = global i32 0
@internal_var = internal global i32 0

define void @legacy() { ret void }
define void @wrapper() pagerando_wrapper { ret void }
define hidden void @binned() pagerando_binned { ret void }

; CHECK-LABEL: # *** IR Dump Before Pagerando intra-bin optimizer for ARM ***:
; CHECK-LABEL: # Machine code for function user: IsSSA, TracksLiveness
; CHECK-LABEL: Constant Pool:
; CHECK: cp#0: legacy(got_brel), align=4
; CHECK: cp#1: wrapper(got_brel), align=4
; CHECK: cp#2: binned(potoff), align=4
; CHECK: cp#3: binned(binoff), align=4
; CHECK: cp#4: global_var(got_brel), align=4
; CHECK: cp#5: internal_var(gotoff), align=4

define void @user() pagerando_binned {
; CHECK: %vreg0<def> = COPY %R9
; CHECK: %vreg1<def> = LDRi12 %vreg0, 0, pred:14, pred:%noreg

; CHECK: %vreg2<def> = LDRi12 <cp#0>, 0, pred:14, pred:%noreg
; CHECK: %vreg3<def> = LDRrs %vreg1, %vreg2<kill>, 0, pred:14, pred:%noreg
; CHECK: BLX %vreg3<kill>
  call void @legacy()

; CHECK: %vreg4<def> = LDRi12 <cp#1>, 0, pred:14, pred:%noreg
; CHECK: %vreg5<def> = LDRrs %vreg1, %vreg4<kill>, 0, pred:14, pred:%noreg
; CHECK: BLX %vreg5<kill>
  call void @wrapper()

; CHECK: %vreg6<def> = LDRi12 <cp#2>, 0, pred:14, pred:%noreg
; CHECK: %vreg7<def> = LDRrs %vreg0, %vreg6<kill>, 0, pred:14, pred:%noreg
; CHECK: %vreg8<def> = LDRi12 <cp#3>, 0, pred:14, pred:%noreg
; CHECK: %vreg9<def> = ADDrr %vreg7<kill>, %vreg8<kill>, pred:14, pred:%noreg, opt:%noreg
; CHECK: BLX %vreg9<kill>
  call void @binned()

; CHECK: %vreg10<def> = LDRi12 <cp#4>, 0, pred:14, pred:%noreg
; CHECK: %vreg11<def> = LDRrs %vreg1, %vreg10<kill>, 0, pred:14, pred:%noreg
  %val = load i32, i32* @global_var

; CHECK: %vreg12<def> = LDRi12 %vreg11<kill>, 0, pred:14, pred:%noreg
; CHECK: %vreg13<def> = LDRi12 <cp#5>, 0, pred:14, pred:%noreg
; CHECK: STRrs %vreg12<kill>, %vreg1, %vreg13<kill>, 0, pred:14, pred:%noreg
  store i32 %val, i32* @internal_var

  ret void
}
