; RUN: opt < %s -pagerando-wrappers -S | FileCheck %s

; CHECK-LABEL: %struct.va_list = type { i8*, i8*, i8*, i32, i32 }

declare void @llvm.va_start(i8*)
declare void @llvm.va_end(i8*)
declare void @llvm.va_copy(i8*, i8*)

define internal void @user() {
  call i32 (...) @varags(i32 13, i32 37)
  ret void
}

; CHECK-DAG: define i32 @varags(...) 
; CHECK-DAG:    %1 = alloca i8
; CHECK-DAG:    call void @llvm.va_start(i8* %1)
; CHECK-DAG:    %2 = call i32 @"varags$$origva"(i8* %1)
; CHECK-DAG:    call void @llvm.va_end(i8* %1)
; CHECK-DAG:    ret i32 %2

; CHECK-DAG: define hidden i32 @"varags$$origva"(i8*)
; CHECK-DAG:    %va = alloca i8
; CHECK-DAG:    call void @llvm.va_copy(i8* %va, i8* %0)
; CHECK-DAG:    %ret = va_arg i8* %va, i32
; CHECK-DAG:    call void @llvm.va_end(i8* %va)
; CHECK-DAG:    ret i32 %ret
define i32 @varags(...) pagerando {
  %va = alloca i8
  call void @llvm.va_start(i8* %va)
  %ret = va_arg i8* %va, i32
  call void @llvm.va_end(i8* %va)
  ret i32 %ret
}

; CHECK-DAG: define void @multiple_starts(...) 
; CHECK-DAG:    %1 = alloca i8
; CHECK-DAG:    call void @llvm.va_start(i8* %1)
; CHECK-DAG:    call void @"multiple_starts$$origva"(i8* %1)
; CHECK-DAG:    call void @llvm.va_end(i8* %1)
; CHECK-DAG:    ret void

; CHECK-DAG: define hidden void @"multiple_starts$$origva"(i8*)
; CHECK-DAG:    %va1 = alloca i8
; CHECK-DAG:    %va2 = alloca i8
; CHECK-DAG:    call void @llvm.va_copy(i8* %va1, i8* %0)
; CHECK-DAG:    call void @llvm.va_copy(i8* %va2, i8* %0)
; CHECK-DAG:    call void @llvm.va_end(i8* %va1)
; CHECK-DAG:    call void @llvm.va_end(i8* %va2)
; CHECK-DAG:    ret void
define void @multiple_starts(...) pagerando {
  %va1 = alloca i8
  %va2 = alloca i8
  call void @llvm.va_start(i8* %va1)
  call void @llvm.va_start(i8* %va2)
  call void @llvm.va_end(i8* %va1)
  call void @llvm.va_end(i8* %va2)
  ret void
}

; CHECK-DAG: define void @copy(...)
; CHECK-DAG:    %1 = alloca i8
; CHECK-DAG:    call void @llvm.va_start(i8* %1)
; CHECK-DAG:    call void @"copy$$origva"(i8* %1)
; CHECK-DAG:    call void @llvm.va_end(i8* %1)
; CHECK-DAG:    ret void

; CHECK-DAG: define hidden void @"copy$$origva"(i8*)
; CHECK-DAG:    %va1 = alloca i8
; CHECK-DAG:    %va2 = alloca i8
; CHECK-DAG:    call void @llvm.va_copy(i8* %va1, i8* %0)
; CHECK-DAG:    call void @llvm.va_copy(i8* %va2, i8* %va1)
; CHECK-DAG:    call void @llvm.va_end(i8* %va1)
; CHECK-DAG:    call void @llvm.va_end(i8* %va2)
; CHECK-DAG:    ret void
define void @copy(...) pagerando {
  %va1 = alloca i8
  %va2 = alloca i8
  call void @llvm.va_start(i8* %va1)
  call void @llvm.va_copy(i8* %va2, i8* %va1)
  call void @llvm.va_end(i8* %va1)
  call void @llvm.va_end(i8* %va2)
  ret void
}

%struct.va_list = type { i8*, i8*, i8*, i32, i32 }

; CHECK-DAG: define void @real_va_type(...)
; CHECK-DAG:    %1 = alloca %struct.va_list
; CHECK-DAG:    %2 = bitcast %struct.va_list* %1 to i8*
; CHECK-DAG:    call void @llvm.va_start(i8* %2)
; CHECK-DAG:    call void @"real_va_type$$origva"(%struct.va_list* %1)
; CHECK-DAG:    %3 = bitcast %struct.va_list* %1 to i8*
; CHECK-DAG:    call void @llvm.va_end(i8* %3)
; CHECK-DAG:    ret void

; CHECK-DAG: define hidden void @"real_va_type$$origva"(%struct.va_list*)
; CHECK-DAG:    %va = alloca %struct.va_list
; CHECK-DAG:    %va_ptr = bitcast %struct.va_list* %va to i8*
; CHECK-DAG:    %2 = bitcast %struct.va_list* %0 to i8*
; CHECK-DAG:    call void @llvm.va_copy(i8* %va_ptr, i8* %2)
; CHECK-DAG:    call void @llvm.va_end(i8* %va_ptr)
; CHECK-DAG:    ret void
define void @real_va_type(...) pagerando {
  %va = alloca %struct.va_list
  %va_ptr = bitcast %struct.va_list* %va to i8*
  call void @llvm.va_start(i8* %va_ptr)
  call void @llvm.va_end(i8* %va_ptr)
  ret void
}
