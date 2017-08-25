; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

@fn_pointer = global void ()* @internal

; CHECK-LABEL: @fn_pointer = global void ()* @"internal$$wrap"

define internal void @internal() { ret void }

; CHECK-LABEL: define internal void @internal() #0
; CHECK-LABEL: define internal void @"internal$$wrap"() #1
