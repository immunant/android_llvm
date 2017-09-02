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
; CHECK-DAG: [[POT:%vreg[0-9]+]]<def> = COPY %R9
; CHECK-DAG: [[GOT:%vreg[0-9]+]]<def> = LDRi12 [[POT]], 0, pred:14, pred:%noreg

; CHECK-DAG: [[LEGACY_GOT:%vreg[0-9]+]]<def> = LDRi12 <cp#0>, 0, pred:14, pred:%noreg
; CHECK: [[LEGACY:%vreg[0-9]+]]<def> = LDRrs [[GOT]], [[LEGACY_GOT]]<kill>, 0, pred:14, pred:%noreg
; CHECK: BLX [[LEGACY]]<kill>
  call void @legacy()

; CHECK: [[WRAPPER_GOT:%vreg[0-9]+]]<def> = LDRi12 <cp#1>, 0, pred:14, pred:%noreg
; CHECK: [[WRAPPER:%vreg[0-9]+]]<def> = LDRrs [[GOT]], [[WRAPPER_GOT]]<kill>, 0, pred:14, pred:%noreg
; CHECK: BLX [[WRAPPER]]<kill>
  call void @wrapper()

; CHECK: [[BINNED_POTOFF:%vreg[0-9]+]]<def> = LDRi12 <cp#2>, 0, pred:14, pred:%noreg
; CHECK: [[BINNED_BIN:%vreg[0-9]+]]<def> = LDRrs [[POT]], [[BINNED_POTOFF]]<kill>, 0, pred:14, pred:%noreg
; CHECK: [[BINNED_BINOFF:%vreg[0-9]+]]<def> = LDRi12 <cp#3>, 0, pred:14, pred:%noreg
; CHECK: [[BINNED:%vreg[0-9]+]]<def> = ADDrr [[BINNED_BIN]]<kill>, [[BINNED_BINOFF]]<kill>, pred:14, pred:%noreg, opt:%noreg
; CHECK: BLX [[BINNED]]<kill>
  call void @binned()

; CHECK: [[GLOBAL_VAR_GOT:%vreg[0-9]+]]<def> = LDRi12 <cp#4>, 0, pred:14, pred:%noreg
; CHECK: [[GLOBAL_VAR:%vreg[0-9]+]]<def> = LDRrs [[GOT]], [[GLOBAL_VAR_GOT]]<kill>, 0, pred:14, pred:%noreg
; CHECK-DAG: [[VAL:%vreg[0-9]+]]<def> = LDRi12 [[GLOBAL_VAR]]<kill>, 0, pred:14, pred:%noreg
  %val = load i32, i32* @global_var

; CHECK-DAG: [[INTERNAL_VAR_GOTOFF:%vreg[0-9]+]]<def> = LDRi12 <cp#5>, 0, pred:14, pred:%noreg
; CHECK: STRrs [[VAL]]<kill>, [[GOT]], [[INTERNAL_VAR_GOTOFF]]<kill>, 0, pred:14, pred:%noreg
  store i32 %val, i32* @internal_var

  ret void
}
