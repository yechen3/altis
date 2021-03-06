////////////////////////////////////////////////////////////////////////////////////////////////////
// file:	altis\src\common\Matrix2DStatics.cpp
//
// summary:	Implements the matrix 2D statics class
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Matrix2D.h"

template<> PMSMemMgr<float>* Matrix2D<float>::pmsmm = 0;
template<> PMSMemMgr<double>* Matrix2D<double>::pmsmm = 0;

