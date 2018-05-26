; RUN: llc < %s -mtriple=armv7-linux -relocation-model=pip -o - | FileCheck %s

; CHECK-LABEL: .section .text.bin_1
; CHECK-LABEL: .type wrapper
; CHECK-LABEL: wrapper:
define void @wrapper() pagerando { ret void }

; CHECK-LABEL: .type  orig
; CHECK-LABEL: orig:
define hidden void @orig() pagerando { ret void }

; CHECK-LABEL: .type  user
; CHECK-LABEL: user:
define void @user() pagerando {
  call void @wrapper()

; CHECK-NOT: add
; CHECK: bl orig
  call void @orig()

  ret void

; Should not appear in the constant pool of this function.
; CHECK-NOT: .text.bin_1
}

; CHECK-LABEL: .section .pot
; CHECK-LABEL: _PAGE_OFFSET_TABLE_:
; CHECK-NOT: .text.bin_1
