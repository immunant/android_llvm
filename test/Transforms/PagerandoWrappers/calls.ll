; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

; CHECK-LABEL: define hidden void @"global$$orig"() #0 {
; CHECK-LABEL: define internal void @internal() #0 {

define void @global() { ret void }
define internal void @internal() { ret void }

; CHECK-LABEL: define internal void @user() #0 {
; CHECK-NEXT:    call void @global()
; CHECK-NEXT:    call void @internal()
; CHECK-NEXT:    ret void

define internal void @user() {
  call void @global()
  call void @internal()
  ret void
}

; CHECK-LABEL: define void @global() #1 {

; CHECK-LABEL: attributes #0 = { pagerando_binned }
; CHECK-LABEL: attributes #1 = { noinline optsize pagerando_wrapper }
