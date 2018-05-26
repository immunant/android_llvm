; RUN: llc < %s -march=aarch64 -relocation-model=pip -o - | FileCheck %s

; CHECK-LABEL: section .text.bin_1
; CHECK-LABEL: wrapper:
define void @wrapper() pagerando { ret void }

; CHECK-LABEL: .type orig
; CHECK-LABEL: orig:
define hidden void @orig() pagerando { ret void }

; CHECK-LABEL: user:
define void @user() pagerando {
  call void @wrapper()

; CHECK-NOT: .text.bin_1
  call void @orig()

  ret void
}

; CHECK-LABEL: .section .pot
; CHECK-LABEL: _PAGE_OFFSET_TABLE_:
; CHECK-NOT: .text.bin_1
