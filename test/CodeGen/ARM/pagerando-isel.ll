; RUN: llc < %s -mtriple=armv7-linux -relocation-model=pip -o /dev/null 2>&1 \
; RUN:     -print-before=pagerando-optimizer-arm | FileCheck %s

@global_var = global i32 0
@internal_var = internal global i32 0

define void @legacy() { ret void }
define void @wrapper() { ret void }
define hidden void @binned() pagerando { ret void }

; CHECK-LABEL: # *** IR Dump Before Pagerando intra-bin optimizer for ARM ***:
; CHECK-LABEL: # Machine code for function user: IsSSA, TracksLiveness
; CHECK-LABEL: Constant Pool:
; CHECK: cp#0: legacy(got_brel), align=4
; CHECK: cp#1: wrapper(got_brel), align=4
; CHECK: cp#2: binned(potoff), align=4
; CHECK: cp#3: binned(binoff), align=4
; CHECK: cp#4: global_var(got_brel), align=4
; CHECK: cp#5: internal_var(gotoff), align=4

; CHECK-LABEL: bb.0 (%ir-block.0)
; CHECK-NEXT: [[LEGACY_GOT:%[0-9]+]]:gprnopc = LDRi12 %const.0, 0, 14, $noreg :: (load 4 from constant-pool)
define void @user() pagerando {
; CHECK-DAG: [[POT:%[0-9]+]]:gpr = COPY $r9
; CHECK-DAG: [[GOT:%[0-9]+]]:gpr = LDRi12 [[POT]]:gpr, 0, 14, $noreg :: (load 4 from pot)
; CHECK-DAG: [[LEGACY:%[0-9]+]]:gpr = LDRrs [[GOT]]:gpr, killed [[LEGACY_GOT]]:gprnopc, 0, 14, $noreg :: (load 4 from got)

; CHECK: BLX killed [[LEGACY]]:gpr
  call void @legacy()

; CHECK: [[WRAPPER_GOT:%[0-9]+]]:gprnopc = LDRi12 %const.1, 0, 14, $noreg :: (load 4 from constant-pool)
; CHECK: [[WRAPPER:%[0-9]+]]:gpr = LDRrs [[GOT]]:gpr, killed [[WRAPPER_GOT]]
; CHECK: BLX killed [[WRAPPER]]:gpr
  call void @wrapper()
; CHECK: [[BINNED_POTOFF:%[0-9]+]]:gprnopc = LDRi12 %const.2, 0, 14, $noreg :: (load 4 from constant-pool) 
; CHECK: [[BINNED_BIN:%[0-9]+]]:gpr = LDRrs [[POT]]:gpr, killed [[BINNED_POTOFF]]:gprnopc, 0, 14, $noreg
; CHECK: [[BINNED_BINOFF:%[0-9]+]]:gpr = LDRi12 %const.3, 0, 14, $noreg :: (load 4 from constant-pool)
; CHECK: [[BINNED:%[0-9]+]]:gpr = ADDrr killed [[BINNED_BIN]]:gpr, killed [[BINNED_BINOFF]]:gpr, 14, $noreg, $noreg
; CHECK: BLX killed [[BINNED]]:gpr
  call void @binned()

; CHECK: [[GLOBAL_VAR_GOT:%[0-9]+]]:gprnopc = LDRi12 %const.4, 0, 14, $noreg :: (load 4 from constant-pool)
; CHECK: [[GLOBAL_VAR:%[0-9]+]]:gpr = LDRrs [[GOT]]:gpr, killed [[GLOBAL_VAR_GOT]]:gprnopc, 0, 14, $noreg
; CHECK-DAG: [[VAL:%[0-9]+]]:gpr = LDRi12 killed [[GLOBAL_VAR]]:gpr, 0, 14, $noreg
  %val = load i32, i32* @global_var

; CHECK-DAG: [[INTERNAL_VAR_GOTOFF:%[0-9]+]]:gprnopc = LDRi12 %const.5, 0, 14, $noreg :: (load 4 from constant-pool)
; CHECK: STRrs killed [[VAL]]:gpr, [[GOT]]:gpr, killed [[INTERNAL_VAR_GOTOFF]]:gprnopc, 0, 14, $noreg
  store i32 %val, i32* @internal_var

  ret void
}
