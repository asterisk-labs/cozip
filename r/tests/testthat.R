# Bootstrap for `R CMD check`. This is the only file in tests/ that
# R discovers automatically; it delegates to testthat which then
# walks tests/testthat/ and runs every test-*.R file there.

library(testthat)
library(cozip)

test_check("cozip")
