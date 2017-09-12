set(LLVM_LINK_COMPONENTS
  AsmPrinter
  CodeGen
  Core
  Support
  )

set(CodeGenSources
  DIEHashTest.cpp
  LowLevelTypeTest.cpp
  MachineInstrBundleIteratorTest.cpp
  PagerandoBinningCallGraphTest.cpp
  PagerandoBinningSimpleTest.cpp
  )

add_llvm_unittest(CodeGenTests
  ${CodeGenSources}
  )

add_subdirectory(GlobalISel)
