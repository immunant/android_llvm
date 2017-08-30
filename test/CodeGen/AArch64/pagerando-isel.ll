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
; CHECK: %vreg0<def> = COPY %X20
; CHECK: %vreg1<def> = LOADpot %vreg0, 0

; CHECK: %vreg2<def> = MOVZXi <ga:@legacy>[TF=166], 0
; CHECK: %vreg3<def,tied1> = MOVKXi %vreg2<tied0>, <ga:@legacy>[TF=165], 16
; CHECK: %vreg4<def,tied1> = MOVKXi %vreg3<tied0>, <ga:@legacy>[TF=164], 32
; CHECK: %vreg5<def,tied1> = MOVKXi %vreg4<tied0>, <ga:@legacy>[TF=35], 48
; CHECK: %vreg6<def> = LOADgotr %vreg1, %vreg5<kill>
; CHECK: BLR %vreg6<kill>
  call void @legacy()

; CHECK: %vreg7<def> = MOVZXi <ga:@wrapper>[TF=166], 0
; CHECK: %vreg8<def,tied1> = MOVKXi %vreg7<tied0>, <ga:@wrapper>[TF=165], 16
; CHECK: %vreg9<def,tied1> = MOVKXi %vreg8<tied0>, <ga:@wrapper>[TF=164], 32
; CHECK: %vreg10<def,tied1> = MOVKXi %vreg9<tied0>, <ga:@wrapper>[TF=35], 48
; CHECK: %vreg11<def> = LOADgotr %vreg1, %vreg10<kill>
; CHECK: BLR %vreg11<kill>
  call void @wrapper()

; CHECK: %vreg12<def> = LOADpot %vreg0, <ga:@binned>[TF=48]
; CHECK: %vreg13<def> = MOVaddrBIN %vreg12<kill>, <ga:@binned>[TF=80]
; CHECK: BLR %vreg13<kill>
  call void @binned()

; CHECK: %vreg14<def> = MOVZXi <ga:@global_var>[TF=166], 0
; CHECK: %vreg15<def,tied1> = MOVKXi %vreg14<tied0>, <ga:@global_var>[TF=165], 16
; CHECK: %vreg16<def,tied1> = MOVKXi %vreg15<tied0>, <ga:@global_var>[TF=164], 32
; CHECK: %vreg17<def,tied1> = MOVKXi %vreg16<tied0>, <ga:@global_var>[TF=35], 48
; CHECK: %vreg18<def> = LOADgotr %vreg1, %vreg17<kill>
; CHECK: %vreg19<def> = LDRWui %vreg18<kill>, 0
  %val = load i32, i32* @global_var

; CHECK: %vreg20<def> = MOVaddrEXT <es:_GLOBAL_OFFSET_TABLE_>[TF=1], <es:_GLOBAL_OFFSET_TABLE_>[TF=130]
; CHECK: %vreg21<def> = MOVaddr <ga:@internal_var>[TF=1], <ga:@internal_var>[TF=130]
; CHECK: %vreg22<def> = SUBXrr %vreg21<kill>, %vreg20<kill>
; CHECK: STRWroX %vreg19<kill>, %vreg1, %vreg22<kill>, 0, 0
  store i32 %val, i32* @internal_var

  ret void
}
