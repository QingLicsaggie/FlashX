% Generated by roxygen2 (4.1.1): do not edit by hand
% Please edit documentation in R/flashmatrix.R
\name{fm.conv.R2FM}
\alias{fm.conv.R2FM}
\title{Convert a regular R object to a FlashMatrix object.}
\usage{
fm.conv.R2FM(obj, byrow = FALSE)
}
\arguments{
\item{obj}{a regular R object}

\item{byrow}{a logical value to determine the data layout of a FlashMatrix
matrix.}
}
\value{
a FlashMatrix object.
}
\description{
If the R object is a matrix, `byrow' determines how data in the generated
FlashMatrix object is organized in memory.
}
\author{
Da Zheng <dzheng5@jhu.edu>
}

