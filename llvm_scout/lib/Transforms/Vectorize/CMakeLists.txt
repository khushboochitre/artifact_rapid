add_llvm_library(LLVMVectorize
  AdditionalVectorization.cpp
  LoadStoreVectorizer.cpp
  LoopVectorizationLegality.cpp
  LoopVectorize.cpp
  SLPVectorizer.cpp
  Vectorize.cpp
  VPlan.cpp
  VPlanHCFGBuilder.cpp
  VPlanHCFGTransforms.cpp
  VPlanPredicator.cpp
  VPlanSLP.cpp
  VPlanVerifier.cpp

  VectorizationCost.cpp
  InstrumentFunction.cpp
  CustomVectorization.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms

  DEPENDS
  intrinsics_gen
  )
