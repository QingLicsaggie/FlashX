% Generated by roxygen2 (4.1.1): do not edit by hand
% Please edit documentation in R/flashgraph.R
\name{fg.graph.info}
\alias{fg.ecount}
\alias{fg.graph.info}
\alias{fg.in.mem}
\alias{fg.is.directed}
\alias{fg.vcount}
\title{Graph information}
\usage{
fg.vcount(graph)

fg.ecount(graph)

fg.in.mem(graph)

fg.is.directed(graph)
}
\arguments{
\item{The}{FlashGraph object}
}
\value{
`fg.vcount' and `fg.ecount' returns integer constants.
`fg.in.mem' and `fg.is.directed' returns boolean constants.
}
\description{
Functions for providing the basic information of a graph.
}
\details{
`fg.vcount' gets the number of vertices in a graph.

`fg.ecount' gets the number of edges in a graph.

`fg.in.mem' indicates whether a graph is stored in memory.

`fg.is.directed' indicates whether a graph is directed.
}
\author{
Da Zheng <dzheng5@jhu.edu>
}

