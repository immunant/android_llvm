; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

; CHECK-LABEL: @fn_ptr1 = global void ()* @global
; CHECK-LABEL: @fn_ptr2 = global void ()* @internal

@fn_ptr1 = global void ()* @global
@fn_ptr2 = global void ()* @internal

; CHECK-LABEL: define internal void @internal() #0
; CHECK-LABEL: define void @global() #0

define void @global() pagerando { ret void }
define internal void @internal() pagerando { ret void }

; CHECK-LABEL: define hidden void @"global$$orig"() #1
; CHECK-LABEL: define internal void @"internal$$orig"() #1