; RUN: llc < %s -mtriple=armv7-linux -relocation-model=pip -o - | FileCheck %s

define void @wrapper() pagerando_wrapper { ret void }
define internal void @orig() pagerando_binned { ret void }

define internal void @user() pagerando_binned {
  call void @wrapper()
  call void @orig()
  ret void
}

; CHECK-LABEL: .text
; CHECK-LABEL: wrapper:

; CHECK-LABEL: section .text.bin_1
; CHECK-LABEL: orig:
; CHECK-LABEL: user:
; CHECK-NOT: add
; CHECK: bl orig
; CHECK-NOT: .text.bin_1

; CHECK-LABEL: .section .pot
; CHECK-LABEL: _PAGE_OFFSET_TABLE_:
; CHECK-NOT: .text.bin_1
