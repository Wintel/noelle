/*
 * Copyright 2016 - 2021  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "IVStepperUtility.hpp"

namespace llvm::noelle{

LoopGoverningIVUtility::LoopGoverningIVUtility (LoopGoverningIVAttribution &attribution)
  : attribution{attribution}
  , conditionValueOrderedDerivation{}
  , flipOperandsToUseNonStrictPredicate{false}
  , flipBrSuccessorsToUseNonStrictPredicate{false} 
{

  /*
   * Fetch the IV
   */
  auto IV = attribution.getInductionVariable();

  /*
   * Fetch information about the condition to exit the loop.
   *
   * Check where the IV is in the comparison (left or right).
   */
  this->condition = attribution.getHeaderCmpInst();
  // TODO: Refer to whichever intermediate value is used in the comparison (known on attribution)
  this->doesOriginalCmpInstHaveIVAsLeftOperand = condition->getOperand(0) == attribution.getValueToCompareAgainstExitConditionValue();

  /*
   * Collect the set of instructions that need to be executed to evaluate the loop exit condition for the subsequent iteration.
   */
  auto conditionValueDerivationSet = attribution.getConditionValueDerivation();
  for (auto &I : *condition->getParent()) {
    if (conditionValueDerivationSet.find(&I) == conditionValueDerivationSet.end()) continue;
    conditionValueOrderedDerivation.push_back(&I);
  }
  assert(IV.getSingleComputedStepValue() && isa<ConstantInt>(IV.getSingleComputedStepValue()));

  /*
   * Fetch information about the step value for the IV.
   */
  auto isStepValuePositive = IV.isStepValuePositive();

  /*
   * Fetch information about the predicate that when true the execution needs to leave the loop.
   */
  auto conditionExitsOnTrue = attribution.getHeaderBrInst()->getSuccessor(0) == attribution.getExitBlockFromHeader();
  auto exitPredicate = conditionExitsOnTrue ? condition->getPredicate() : condition->getInversePredicate();
  exitPredicate = doesOriginalCmpInstHaveIVAsLeftOperand ? exitPredicate : CmpInst::getSwappedPredicate(exitPredicate);
  this->flipOperandsToUseNonStrictPredicate = !doesOriginalCmpInstHaveIVAsLeftOperand;
  this->flipBrSuccessorsToUseNonStrictPredicate = !conditionExitsOnTrue;
  this->nonStrictPredicate = exitPredicate;
  this->strictPredicate = exitPredicate;
  switch (exitPredicate) {
    case CmpInst::Predicate::ICMP_NE:

      /*
       * This predicate is non-strict and will result in either 0 or 1 iteration(s)
       */
      break;
    case CmpInst::Predicate::ICMP_EQ:
      // This predicate is strict and needs to be extended to LTE/GTE to catch jumping past the exiting value
      if (isStepValuePositive){
        this->nonStrictPredicate = CmpInst::Predicate::ICMP_SGE;
        this->strictPredicate = CmpInst::Predicate::ICMP_SGT;
      } else {
        this->nonStrictPredicate = CmpInst::Predicate::ICMP_SLE;
        this->strictPredicate = CmpInst::Predicate::ICMP_SLT;
      }
      break;
    case CmpInst::Predicate::ICMP_SLE:
    case CmpInst::Predicate::ICMP_SLT:
    case CmpInst::Predicate::ICMP_ULT:
    case CmpInst::Predicate::ICMP_ULE:
      // This predicate is non-strict. We simply assert that the step value has the expected sign

      // HACK: while it is technically correct to increment with a less than exit condition, yielding 0 or 1 iteration,
      // it would break under assumptions that further recurrences of the IV can be checked on this condition
      // Our parallelization schemes make that assumption, hence the assert here
      assert(!isStepValuePositive && "IV step value is not compatible with exit condition!");
      break;
    case CmpInst::Predicate::ICMP_UGT:
    case CmpInst::Predicate::ICMP_UGE:
    case CmpInst::Predicate::ICMP_SGT:
    case CmpInst::Predicate::ICMP_SGE:
      // This predicate is non-strict. We simply assert that the step value has the expected sign

      // HACK: while it is technically correct to decrement with a greater than exit condition, yielding 0 or 1 iteration,
      // it would break under assumptions that further recurrences of the IV can be checked on this condition
      // Our parallelization schemes make that assumption, hence the assert here
      assert(isStepValuePositive && "IV step value is not compatible with exit condition!");
      break;
  }

  return ;
}

void LoopGoverningIVUtility::updateConditionAndBranchToCatchIteratingPastExitValue(
  CmpInst *cmpToUpdate,
  BranchInst *branchInst,
  BasicBlock *exitBlock
  ) {

  if (flipOperandsToUseNonStrictPredicate) {
    auto opL = cmpToUpdate->getOperand(0);
    auto opR = cmpToUpdate->getOperand(1);
    cmpToUpdate->setOperand(0, opR);
    cmpToUpdate->setOperand(1, opL);
  }
  cmpToUpdate->setPredicate(this->nonStrictPredicate);

  if (flipBrSuccessorsToUseNonStrictPredicate) {
    branchInst->swapSuccessors();
  }

  return ;
}

void LoopGoverningIVUtility::cloneConditionalCheckFor(
  Value *recurrenceOfIV,
  Value *clonedCompareValue,
  BasicBlock *continueBlock,
  BasicBlock *exitBlock,
  IRBuilder<> &cloneBuilder
  ) {

  /*
   * Create the comparison instruction.
   */
  auto cmpInst = cloneBuilder.CreateICmp(this->nonStrictPredicate, recurrenceOfIV, clonedCompareValue);

  /*
   * Add the conditional branch
   */
  cloneBuilder.CreateCondBr(cmpInst, exitBlock, continueBlock);

  return ;
}

void LoopGoverningIVUtility::updateConditionToCheckIfWeHavePastExitValue(
  CmpInst *cmpToUpdate
  ){
  auto IV = this->attribution.getInductionVariable();
  if (this->attribution.getValueToCompareAgainstExitConditionValue() != IV.getLoopEntryPHI()){
    cmpToUpdate->setPredicate(this->strictPredicate);
  }

  return ;
}

std::vector<Instruction *> & LoopGoverningIVUtility::getConditionValueDerivation (void) {
  return conditionValueOrderedDerivation;
}

Value * LoopGoverningIVUtility::generateCodeToComputeTheTripCount (
  IRBuilder<> &builder
  ){

  /*
   * Fetch the start and last value.
   */
  auto IV = this->attribution.getInductionVariable();
  auto startValue = IV.getStartValue();
  auto lastValue = this->attribution.getExitConditionValue();

  /*
   * Compute the delta.
   */
  Value *delta = nullptr;
  if (IV.isStepValuePositive()){
    delta = builder.CreateSub(lastValue, startValue);
  } else {
    delta = builder.CreateSub(startValue, lastValue);
  }

  /*
   * Compute the number of steps to reach the delta.
   */
  auto tripCount = builder.CreateUDiv(delta, IV.getSingleComputedStepValue());

  return tripCount;
}

Value * LoopGoverningIVUtility::generateCodeToComputePreviousValueUsedToCompareAgainstExitConditionValue (
  IRBuilder<> &builder,
  Value *currentIterationValue,
  BasicBlock *latch,
  Value *stepValue
  ){

  /*
   * Assert that the builder is pointing to an instruction within the loop.
   */
  //TODO

  /*
   * Generate the value that was used to compare against the exit condition value in the last iteration.
   */ 
  auto prevIterationValue = this->generateCodeToComputeValueToUseForAnIterationAgo(builder, currentIterationValue, stepValue);

  return prevIterationValue;
}

Value * LoopGoverningIVUtility::generateCodeToComputeValueToUseForAnIterationAgo (
  IRBuilder<> &builder,
  Value *currentIterationValue,
  Value *stepValue
  ){

  /*
   * Check if the value used to compare against the exit condition value is the PHI of the loop governing IV.
   */
  auto &IV = this->attribution.getInductionVariable();
  if (this->attribution.getValueToCompareAgainstExitConditionValue() == IV.getLoopEntryPHI()){

    /*
     * The value used is the PHI.
     * Hence, we must generate code to compute the value of the previous iteration.
     */
    auto prevIterationValue = builder.CreateSub(currentIterationValue, stepValue);

    return prevIterationValue;
  }

  /*
   * The value used to check whether we should exit the loop is the updated value.
   * Hence, the previous value is simply the current updated one.
   */
  return currentIterationValue;
}

}
