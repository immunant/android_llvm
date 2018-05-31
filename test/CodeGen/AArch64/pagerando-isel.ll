; RUN: llc < %s -march=aarch64 -relocation-model=pip -o /dev/null 2>&1 \
; RUN:     -print-before=pagerando-optimizer-aarch64 | FileCheck %s

@global_var = global i32 0
@internal_var = internal global i32 0

define void @legacy() { ret void }
define void @wrapper() { ret void }
define hidden void @binned() pagerando { ret void }

; CHECK-LABEL: # *** IR Dump Before Pagerando intra-bin optimizer for AArch64 ***:
; CHECK-LABEL: # Machine code for function user: IsSSA, TracksLiveness
define void @user() pagerando {
; CHECK-DAG: [[POT:%[0-9]+]]:gpr64 = COPY $x20
; CHECK-DAG: [[GOT:%[0-9]+]]:gpr64common = LOADpot [[POT]]:gpr64, 0

; CHECK-DAG: %{{[0-9]+}}:gpr64 = MOVZXi target-flags(aarch64-g0, aarch64-nc, aarch64-gotoff) @legacy, 0
; CHECK-DAG: %{{[0-9]+}}:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g1, aarch64-nc, aarch64-gotoff) @legacy, 16
; CHECK-DAG: %{{[0-9]+}}:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g2, aarch64-nc, aarch64-gotoff) @legacy, 32
; CHECK-DAG: [[LEGACY_GOTOFF:%[0-9]+]]:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g3, aarch64-gotoff) @legacy, 48
; CHECK: [[LEGACY:%[0-9]+]]:gpr64 = LOADgotr [[GOT]]:gpr64common, killed [[LEGACY_GOTOFF]]:gpr64
; CHECK: BLR killed [[LEGACY]]:gpr64
  call void @legacy()

; CHECK: %{{[0-9]+}}:gpr64 = MOVZXi target-flags(aarch64-g0, aarch64-nc, aarch64-gotoff) @wrapper, 0
; CHECK: %{{[0-9]+}}:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g1, aarch64-nc, aarch64-gotoff) @wrapper, 16
; CHECK: %{{[0-9]+}}:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g2, aarch64-nc, aarch64-gotoff) @wrapper, 32
; CHECK: [[WRAPPER_GOTOFF:%[0-9]+]]:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g3, aarch64-gotoff) @wrapper, 48
; CHECK: [[WRAPPER:%[0-9]+]]:gpr64 = LOADgotr [[GOT]]:gpr64common, killed [[WRAPPER_GOTOFF]]:gpr64
; CHECK: BLR killed [[WRAPPER]]
  call void @wrapper()

; CHECK: [[BINNED_BIN:%[0-9]+]]:gpr64 = LOADpot [[POT]]:gpr64, target-flags(aarch64-got, aarch64-gotoff) @binned
; CHECK: [[BINNED:%[0-9]+]]:gpr64 = MOVaddrBIN killed [[BINNED_BIN]]:gpr64, target-flags(aarch64-got, aarch64-tls) @binned
; CHECK: BLR killed [[BINNED]]:gpr64
  call void @binned()

; CHECK: %{{[0-9]+}}:gpr64 = MOVZXi target-flags(aarch64-g0, aarch64-nc, aarch64-gotoff) @global_var, 0
; CHECK: %{{[0-9]+}}:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g1, aarch64-nc, aarch64-gotoff) @global_var, 16
; CHECK: %{{[0-9]+}}:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g2, aarch64-nc, aarch64-gotoff) @global_var, 32
; CHECK: [[GLOBAL_VAR_GOTOFF:%[0-9]+]]:gpr64 = MOVKXi %{{[0-9]+}}:gpr64, target-flags(aarch64-g3, aarch64-gotoff) @global_var, 48
; CHECK: [[GLOBAL_VAR_ADDR:%[0-9]+]]:gpr64common = LOADgotr [[GOT]]:gpr64common, killed [[GLOBAL_VAR_GOTOFF]]:gpr64
; CHECK: [[VAL:%[0-9]+]]:gpr32 = LDRWui killed [[GLOBAL_VAR_ADDR]]:gpr64common, 0
  %val = load i32, i32* @global_var

; CHECK: [[GOT_PCREL:%[0-9]+]]:gpr64 = MOVaddrEXT target-flags(aarch64-page) &_GLOBAL_OFFSET_TABLE_, target-flags(aarch64-pageoff, aarch64-nc) &_GLOBAL_OFFSET_TABLE_
; CHECK: [[INTERNAL_VAR_PCREL:%[0-9]+]]:gpr64 = MOVaddr target-flags(aarch64-page) @internal_var, target-flags(aarch64-pageoff, aarch64-nc) @internal_var
; CHECK: [[DIFF:%[0-9]+]]:gpr64 = SUBXrr killed [[INTERNAL_VAR_PCREL]]:gpr64, killed [[GOT_PCREL]]:gpr64
; CHECK: STRWroX killed [[VAL]]:gpr32, [[GOT]]:gpr64common, killed [[DIFF]]:gpr64, 0, 0
  store i32 %val, i32* @internal_var

  ret void
}