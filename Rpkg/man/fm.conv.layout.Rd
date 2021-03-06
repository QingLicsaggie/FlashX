% Generated by roxygen2 (4.1.1): do not edit by hand
% Please edit documentation in R/flashmatrix.R
\name{fm.conv.layout}
\alias{fm.conv.layout}
\title{Convert the data layout of a FlashMatrix matrix.}
\usage{
fm.conv.layout(fm, byrow = FALSE)
}
\arguments{
\item{fm}{a FlashMatrix matrix}

\item{byrow}{a logical value to determine the data layout of a FlashMatrix
matrix.}
}
\value{
a FlashMatrix matrix
}
\description{
A FMR matrix can store elements in a row-major or column-major order.
By changing the data layout, we can improve efficiency of some matrix
operations.
}
\author{
Da Zheng <dzheng5@jhu.edu>
}

