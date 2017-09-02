; RUN: llc < %s -march=aarch64 -relocation-model=pip -o /dev/null 2>&1 \
; RUN:     -print-before=pagerando-optimizer-aarch64 | FileCheck %s

@global_var = global i32 0
@internal_var = internal global i32 0

define void @legacy() { ret void }
define void @wrapper() pagerando_wrapper { ret void }
define hidden void @binned() pagerando_binned { ret void }

; CHECK-LABEL: # *** IR Dump Before Pagerando intra-bin optimizer for AArch64 ***:
; CHECK-LABEL: # Machine code for function user: IsSSA, TracksLiveness
define void @user() pagerando_binned {
; CHECK-DAG: [[POT:%vreg[0-9]+]]<def> = COPY %X20
; CHECK-DAG: [[GOT:%vreg[0-9]+]]<def> = LOADpot [[POT]], 0

; CHECK-DAG: %vreg{{[0-9]+}}<def> = MOVZXi <ga:@legacy>[TF=166], 0
; CHECK-DAG: %vreg{{[0-9]+}}<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@legacy>[TF=165], 16
; CHECK-DAG: %vreg{{[0-9]+}}<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@legacy>[TF=164], 32
; CHECK-DAG: [[LEGACY_GOTOFF:%vreg[0-9]+]]<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@legacy>[TF=35], 48
; CHECK: [[LEGACY:%vreg[0-9]+]]<def> = LOADgotr [[GOT]], [[LEGACY_GOTOFF]]<kill>
; CHECK: BLR [[LEGACY]]<kill>
  call void @legacy()

; CHECK: %vreg{{[0-9]+}}<def> = MOVZXi <ga:@wrapper>[TF=166], 0
; CHECK: %vreg{{[0-9]+}}<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@wrapper>[TF=165], 16
; CHECK: %vreg{{[0-9]+}}<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@wrapper>[TF=164], 32
; CHECK: [[WRAPPER_GOTOFF:%vreg[0-9]+]]<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@wrapper>[TF=35], 48
; CHECK: [[WRAPPER:%vreg[0-9]+]]<def> = LOADgotr [[GOT]], [[WRAPPER_GOTOFF]]<kill>
; CHECK: BLR [[WRAPPER]]<kill>
  call void @wrapper()

; CHECK: [[BINNED_BIN:%vreg[0-9]+]]<def> = LOADpot [[POT]], <ga:@binned>[TF=48]
; CHECK: [[BINNED:%vreg[0-9]+]]<def> = MOVaddrBIN [[BINNED_BIN]]<kill>, <ga:@binned>[TF=80]
; CHECK: BLR [[BINNED]]<kill>
  call void @binned()

; CHECK: %vreg{{[0-9]+}}<def> = MOVZXi <ga:@global_var>[TF=166], 0
; CHECK: %vreg{{[0-9]+}}<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@global_var>[TF=165], 16
; CHECK: %vreg{{[0-9]+}}<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@global_var>[TF=164], 32
; CHECK: [[GLOBAL_VAR_GOTOFF:%vreg[0-9]+]]<def,tied1> = MOVKXi %vreg{{[0-9]+}}<tied0>, <ga:@global_var>[TF=35], 48
; CHECK: [[GLOBAL_VAR_ADDR:%vreg[0-9]+]]<def> = LOADgotr [[GOT]], [[GLOBAL_VAR_GOTOFF]]<kill>
; CHECK: [[VAL:%vreg[0-9]+]]<def> = LDRWui [[GLOBAL_VAR_ADDR]]<kill>, 0
  %val = load i32, i32* @global_var

; CHECK: [[GOT_PCREL:%vreg[0-9]+]]<def> = MOVaddrEXT <es:_GLOBAL_OFFSET_TABLE_>[TF=1], <es:_GLOBAL_OFFSET_TABLE_>[TF=130]
; CHECK: [[INTERNAL_VAR_PCREL:%vreg[0-9]+]]<def> = MOVaddr <ga:@internal_var>[TF=1], <ga:@internal_var>[TF=130]
; CHECK: [[DIFF:%vreg[0-9]+]]<def> = SUBXrr [[INTERNAL_VAR_PCREL]]<kill>, [[GOT_PCREL]]<kill>
; CHECK: STRWroX [[VAL]]<kill>, [[GOT]], [[DIFF]]<kill>, 0, 0
  store i32 %val, i32* @internal_var

  ret void
}
