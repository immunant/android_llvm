; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

@fn_ptr1 = global void ()* @global
@fn_ptr2 = global void ()* @internal

; CHECK-LABEL: @fn_ptr1 = global void ()* @global
; CHECK-LABEL: @fn_ptr2 = global void ()* @internal

define void @global() { ret void }

define internal void @internal() { ret void }

; CHECK-LABEL: define hidden void @"global$$orig"() #0
; CHECK-LABEL: define internal void @"internal$$orig"() #0

; CHECK-LABEL: define void @global() #1
; CHECK-LABEL: define internal void @internal() #1
