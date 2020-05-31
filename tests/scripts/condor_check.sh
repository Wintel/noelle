#!/bin/bash

function identifyElementsOutsideSet {
  local setFile="$1" ;
  local elementsToCheckFile="$2" ;

  outsideElements="" ;
  while IFS= read -r line; do
    grep -Fxq "$line" $setFile ;
    if test $? -ne 0 ; then
      outsideElements="${outsideElements}\n\t$line" ;
    fi
  done < "$elementsToCheckFile" 

  return 0;
}


echo "Checking the regression test results" ;

# Check the tests that are still running
stillRunning="`mktemp`" ;
condor_q `whoami` -l | grep ^Arguments > $stillRunning ;
if test -s $stillRunning ; then
  echo "  The following tests are still running" ;
  while IFS= read -r line; do
    testRunning=`echo $line | awk '{print $4}'` ;
    echo "    $testRunning" ;
  done < "$stillRunning"

else
  echo "  All tests finished" ;
fi

# Local variables
origDir="`pwd`" ;
origDirLength=`echo "${#origDir} + 2" | bc` ;

# Fetch the local results
currentResults="`mktemp`" ; 
cat regression*/*.txt  | sort | cut -c ${origDirLength}- > $currentResults ;

# Compare with the known one
newTestsFailed="" ;
while IFS= read -r line; do
  grep -Fxq "$line" failing_tests.txt ;
  if test $? -ne 0 ; then
    newTestsFailed="${newTestsFailed}\n\t$line" ;
  fi
done < "$currentResults"

# Check the results
if test "$newTestsFailed" != "" ; then
  echo -e "  New tests failed: $newTestsFailed" ;
fi

# The regression passed
echo "The regression tests passed" ;
oldTestsNumber=`wc -l failing_tests.txt | awk '{print $1}'` ;
newTestsNumber=`wc -l $currentResults | awk '{print $1}'` ;
if test ${newTestsNumber} == ${oldTestsNumber} ; then
  echo "  All tests that failed before still fail" ;
  exit 0;
fi
lessTests=`echo "${oldTestsNumber} - ${newTestsNumber}" | bc` ;
echo "  There are $lessTests less tests that fail now!" ;

# Print the tests that now pass
echo "  These tests are the following ones:" ;
identifyElementsOutsideSet $currentResults failing_tests.txt ;
echo -e "$outsideElements" ;
