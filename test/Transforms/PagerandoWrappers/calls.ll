; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

; CHECK-LABEL: define void @global() #0 {
; CHECK-LABEL: define hidden void @"global$$orig"() #1 {
; CHECK-LABEL: define internal void @internal() #1 {

define void @global() pagerando { ret void }
define internal void @internal() pagerando { ret void }

; CHECK-LABEL: define internal void @user() #1 {
define internal void @user() pagerando {
; CHECK-NEXT:    call void @global()
; CHECK-NEXT:    call void @internal()
; CHECK-NEXT:    ret void
  call void @global()
  call void @internal()
  ret void
}


; CHECK-LABEL: attributes #0 = { noinline optsize }
; CHECK-LABEL: attributes #1 = { pagerando }