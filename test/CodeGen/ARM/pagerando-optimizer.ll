; RUN: llc < %s -mtriple=armv7-linux -relocation-model=pip -o - | FileCheck %s

; CHECK-LABEL: .text
; CHECK-LABEL: wrapper:
define void @wrapper() pagerando_wrapper { ret void }

; CHECK-LABEL: section .text.bin_1
; CHECK-LABEL: orig:
define hidden void @orig() pagerando_binned { ret void }

; CHECK-LABEL: user:
define void @user() pagerando_binned {
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
