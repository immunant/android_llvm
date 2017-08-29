; RUN: llc < %s -march=aarch64 -relocation-model=pip -o - | FileCheck %s

; CHECK-LABEL: .text
; CHECK-LABEL: wrapper:
define void @wrapper() pagerando_wrapper { ret void }

; CHECK-LABEL: section .text.bin_1
; CHECK-LABEL: orig:
define internal void @orig() pagerando_binned { ret void }


; CHECK-LABEL: user:
define internal void @user() pagerando_binned {
  call void @wrapper()

; CHECK-NOT: .text.bin_1
; CHECK: bl orig
  call void @orig()

  ret void
}

; CHECK-LABEL: .section .pot
; CHECK-LABEL: _PAGE_OFFSET_TABLE_:
; CHECK-NOT: .text.bin_1
