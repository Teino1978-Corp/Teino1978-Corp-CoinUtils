// Copyright (C) 2002, International Business Machines
// Corporation and others.  All Rights Reserved.
#include <stdio.h>
#include <math.h>

#include "CoinPresolveMatrix.hpp"
#include "CoinPresolveSubst.hpp"
#include "CoinPresolveIsolated.hpp"
#include "CoinPresolveImpliedFree.hpp"
#include "CoinMessage.hpp"
#include "CoinHelperFunctions.hpp"
#include "CoinSort.hpp"

// If there is a row with a singleton column such that no matter what
// the values of the other variables are, the constraint forces the singleton
// column to have a feasible value, then we can drop the column and row,
// since we just compute the value of the column from the row in postsolve.
// This seems vaguely similar to the case of a useless constraint, but it
// is different.  For example, if the singleton column is already free,
// then this operation will eliminate it, but the constraint is not useless
// (assuming the constraint is not trivial), since the variables do not imply an
// upper or lower bound.
//
// If the column is not singleton, we can still do something similar if the
// constraint is an equality constraint.  In that case, we substitute away
// the variable in the other constraints it appears in.  This introduces
// new coefficients, but the total number of coefficients never increases
// if the column has only two constraints, and may not increase much even
// if there are more.
//
// There is nothing to prevent us from substituting away a variable
// in an equality from the other constraints it appears in, but since
// that causes fill-in, it wouldn't make sense unless we could then
// drop the equality itself.  We can't do that if the bounds on the
// variable in equation aren't implied by the equality.
// Another way of thinking of this is that there is nothing special
// about an equality; just like one can't always drop an inequality constraint
// with a column singleton, one can't always drop an equality.
//
// It is possible for two singleton columns to be in the same row.
// In that case, the other one will become empty.  If it's bounds and
// costs aren't just right, this signals an unbounded problem.
// We don't need to check that specially here.
//
// invariant:  loosely packed
const CoinPresolveAction *implied_free_action::presolve(CoinPresolveMatrix *prob,
						     const CoinPresolveAction *next,
						    int & fill_level)
{
  double *colels	= prob->colels_;
  int *hrow	= prob->hrow_;
  const CoinBigIndex *mcstrt	= prob->mcstrt_;
  int *hincol	= prob->hincol_;
  const int ncols	= prob->ncols_;

  const double *clo	= prob->clo_;
  const double *cup	= prob->cup_;

  const double *rowels	= prob->rowels_;
  const int *hcol	= prob->hcol_;
  const CoinBigIndex *mrstrt	= prob->mrstrt_;
  int *hinrow	= prob->hinrow_;
  int nrows	= prob->nrows_;

  /*const*/ double *rlo	= prob->rlo_;
  /*const*/ double *rup	= prob->rup_;

  double *cost		= prob->cost_;

  presolvehlink *rlink = prob->rlink_;
  presolvehlink *clink = prob->clink_;

  const char *integerType = prob->integerType_;

  const double tol = prob->feasibilityTolerance_;

  //  int nbounds = 0;

  action *actions	= new action [ncols];
  int nactions = 0;

  int *implied_free = new int[ncols];
  int i;
  for (i=0;i<ncols;i++)
    implied_free[i]=-1;

  // memory for min max
  int * infiniteDown = new int [nrows];
  int * infiniteUp = new int [nrows];
  double * maxDown = new double[nrows];
  double * maxUp = new double[nrows];

  // mark as not computed
  // -1 => not computed, -2 give up (singleton), -3 give up (other)
  for (i=0;i<nrows;i++) {
    if (hinrow[i]>1)
      infiniteUp[i]=-1;
    else
      infiniteUp[i]=-2;
  }
  double large=1.0e20;

  int numberLook = prob->numberColsToDo_;
  int iLook;
  int * look = prob->colsToDo_;
  int * look2 = NULL;
  // if gone from 2 to 3 look at all
  if (fill_level<0) {
    look2 = new int[ncols];
    look=look2;
    for (iLook=0;iLook<ncols;iLook++) 
      look[iLook]=iLook;
    numberLook=ncols;
 }

  for (iLook=0;iLook<numberLook;iLook++) {
    int j=look[iLook];
    if ((hincol[j]  <= 3) && !integerType[j]) {
      if (hincol[j]>1) {
	// extend to > 3 later
	CoinBigIndex kcs = mcstrt[j];
	CoinBigIndex kce = kcs + hincol[j];
	bool possible = false;
	bool singleton = false;
	CoinBigIndex k;
	double largestElement=0.0;
	for (k=kcs; k<kce; ++k) {
	  int row = hrow[k];
	  double coeffj = colels[k];
	  
	  // if its row is an equality constraint...
	  if (hinrow[row] > 1 ) {
	    if ( fabs(rlo[row] - rup[row]) < tol &&
		 fabs(coeffj) > ZTOLDP) {
	      possible=true;
	    }
	    largestElement = max(largestElement,fabs(coeffj));
	  } else {
	    singleton=true;
	  }
	}
	if (possible&&!singleton) {
	  double low=-COIN_DBL_MAX;
	  double high=COIN_DBL_MAX;
	  // get bound implied by all rows
	  for (k=kcs; k<kce; ++k) {
	    int row = hrow[k];
	    double coeffj = colels[k];
	    if (fabs(coeffj) > ZTOLDP) {
	      if (infiniteUp[row]==-1) {
		// compute
		CoinBigIndex krs = mrstrt[row];
		CoinBigIndex kre = krs + hinrow[row];
		int infiniteUpper = 0;
		int infiniteLower = 0;
		double maximumUp = 0.0;
		double maximumDown = 0.0;
		CoinBigIndex kk;
		// Compute possible lower and upper ranges
		for (kk = krs; kk < kre; ++kk) {
		  double value=rowels[kk];
		  int iColumn = hcol[kk];
		  if (value > 0.0) {
		    if (cup[iColumn] >= large) {
		      ++infiniteUpper;
		    } else {
		      maximumUp += cup[iColumn] * value;
		    }
		    if (clo[iColumn] <= -large) {
		      ++infiniteLower;
		    } else {
		      maximumDown += clo[iColumn] * value;
		    }
		  } else if (value<0.0) {
		    if (cup[iColumn] >= large) {
		      ++infiniteLower;
		    } else {
		      maximumDown += cup[iColumn] * value;
		    }
		    if (clo[iColumn] <= -large) {
		      ++infiniteUpper;
		    } else {
		      maximumUp += clo[iColumn] * value;
		    }
		  }
		}
		double maxUpx = maximumUp+infiniteUpper*1.0e31;
		double maxDownx = maximumDown-infiniteLower*1.0e31;
		if (maxUpx <= rup[row] + tol && 
		    maxDownx >= rlo[row] - tol) {
	  
		  // Row is redundant 
		  infiniteUp[row]=-3;

		} else if (maxUpx < rlo[row] -tol ) {
		  /* there is an upper bound and it can't be reached */
		  prob->status_|= 1;
		  prob->messageHandler()->message(COIN_PRESOLVE_ROWINFEAS,
						  prob->messages())
						    <<row
						    <<rlo[row]
						    <<rup[row]
						    <<CoinMessageEol;
		  infiniteUp[row]=-3;
		  break;
		} else if ( maxDownx > rup[row]+tol) {
		  /* there is a lower bound and it can't be reached */
		  prob->status_|= 1;
		  prob->messageHandler()->message(COIN_PRESOLVE_ROWINFEAS,
						  prob->messages())
						    <<row
						    <<rlo[row]
						    <<rup[row]
						    <<CoinMessageEol;
		  infiniteUp[row]=-3;
		  break;
		} else {
		  infiniteUp[row]=infiniteUpper;
		  infiniteDown[row]=infiniteLower;
		  maxUp[row]=maximumUp;
		  maxDown[row]=maximumDown;
		}
	      } 
	      if (infiniteUp[row]>=0) {
		double lower = rlo[row];
		double upper = rup[row];
		double value=coeffj;
		double nowLower = clo[j];
		double nowUpper = cup[j];
		double newBound;
		int infiniteUpper=infiniteUp[row];
		int infiniteLower=infiniteDown[row];
		double maximumUp = maxUp[row];
		double maximumDown = maxDown[row];
		if (value > 0.0) {
		  // positive value
		  if (lower>-large) {
		    if (!infiniteUpper) {
		      assert(nowUpper < large);
		      newBound = nowUpper + 
			(lower - maximumUp) / value;
		      // relax if original was large
		      if (fabs(maximumUp)>1.0e8)
			newBound -= 1.0e-12*fabs(maximumUp);
		    } else if (infiniteUpper==1&&nowUpper>large) {
		      newBound = (lower -maximumUp) / value;
		      // relax if original was large
		      if (fabs(maximumUp)>1.0e8)
			newBound -= 1.0e-12*fabs(maximumUp);
		    } else {
		      newBound = -COIN_DBL_MAX;
		    }
		    if (newBound > nowLower + 1.0e-12) {
		      // Tighten the lower bound 
		      // adjust
		      double now;
		      if (nowLower<-large) {
			now=0.0;
			infiniteLower--;
		      } else {
			now = nowLower;
		      }
		      maximumDown += (newBound-now) * value;
		      nowLower = newBound;
		    }
		    low=max(low,newBound);
		  } 
		  if (upper <large) {
		    if (!infiniteLower) {
		      assert(nowLower >- large);
		      newBound = nowLower + 
			(upper - maximumDown) / value;
		      // relax if original was large
		      if (fabs(maximumDown)>1.0e8)
			newBound += 1.0e-12*fabs(maximumDown);
		    } else if (infiniteLower==1&&nowLower<-large) {
		      newBound =   (upper - maximumDown) / value;
		      // relax if original was large
		      if (fabs(maximumDown)>1.0e8)
			newBound += 1.0e-12*fabs(maximumDown);
		    } else {
		      newBound = COIN_DBL_MAX;
		    }
		    if (newBound < nowUpper - 1.0e-12) {
		      // Tighten the upper bound 
		      // adjust 
		      double now;
		      if (nowUpper>large) {
			now=0.0;
			infiniteUpper--;
		      } else {
			now = nowUpper;
		      }
		      maximumUp += (newBound-now) * value;
		      nowUpper = newBound;
		    }
		    high=min(high,newBound);
		  }
		} else {
		  // negative value
		  if (lower>-large) {
		    if (!infiniteUpper) {
		      assert(nowLower >- large);
		      newBound = nowLower + 
			(lower - maximumUp) / value;
		      // relax if original was large
		      if (fabs(maximumUp)>1.0e8)
			newBound += 1.0e-12*fabs(maximumUp);
		    } else if (infiniteUpper==1&&nowLower<-large) {
		      newBound = (lower -maximumUp) / value;
		      // relax if original was large
		      if (fabs(maximumUp)>1.0e8)
			newBound += 1.0e-12*fabs(maximumUp);
		    } else {
		      newBound = COIN_DBL_MAX;
		    }
		    if (newBound < nowUpper - 1.0e-12) {
		      // Tighten the upper bound 
		      // adjust
		      double now;
		      if (nowUpper>large) {
			now=0.0;
			infiniteLower--;
		      } else {
			now = nowUpper;
		      }
		      maximumDown += (newBound-now) * value;
		      nowUpper = newBound;
		    }
		    high=min(high,newBound);
		  }
		  if (upper <large) {
		    if (!infiniteLower) {
		      assert(nowUpper < large);
		      newBound = nowUpper + 
			(upper - maximumDown) / value;
		      // relax if original was large
		      if (fabs(maximumDown)>1.0e8)
			newBound -= 1.0e-12*fabs(maximumDown);
		    } else if (infiniteLower==1&&nowUpper>large) {
		      newBound =   (upper - maximumDown) / value;
		      // relax if original was large
		      if (fabs(maximumDown)>1.0e8)
			newBound -= 1.0e-12*fabs(maximumDown);
		    } else {
		      newBound = -COIN_DBL_MAX;
		    }
		    if (newBound > nowLower + 1.0e-12) {
		      // Tighten the lower bound 
		      // adjust
		      double now;
		      if (nowLower<-large) {
			now=0.0;
			infiniteUpper--;
		      } else {
			now = nowLower;
		      }
		      maximumUp += (newBound-now) * value;
		      nowLower = newBound;
		    }
		    low = max(low,newBound);
		  }
		}
	      } else if (infiniteUp[row]==-3) {
		// give up
		high=COIN_DBL_MAX;
		low=-COIN_DBL_MAX;
		break;
	      }
	    }
	  }
	  if (clo[j] <= low && high <= cup[j]) {
	      
	    // both column bounds implied by the constraints of the problem
	    // get row
	    largestElement *= 0.1;
	    int krow=-1;
	    int ninrow=ncols+1;
	    for (k=kcs; k<kce; ++k) {
	      int row = hrow[k];
	      double coeffj = colels[k];
	      if ( fabs(rlo[row] - rup[row]) < tol &&
		   fabs(coeffj) > largestElement) {
		if (hinrow[row]<ninrow) {
		  ninrow=hinrow[row];
		  krow=row;
		}
	      }
	    }
	    if (krow>=0) {
	      implied_free[j] = krow;
	      // And say row no good for further use
	      infiniteUp[krow]=-3;
	      //printf("column %d implied free by row %d hincol %d hinrow %d\n",
	      //     j,krow,hincol[j],hinrow[krow]);
	    }
	  }
	}
      } else if (hincol[j]) {
	// singleton column
	CoinBigIndex k = mcstrt[j];
	int row = hrow[k];
	double coeffj = colels[k];
	if ((!cost[j]||rlo[row]==rup[row])&&hinrow[row]>1&&
	    fabs(coeffj) > ZTOLDP&&infiniteUp[row]!=-3) {
	  
	  CoinBigIndex krs = mrstrt[row];
	  CoinBigIndex kre = krs + hinrow[row];
	  
	  double maxup, maxdown, ilow, iup;
	  implied_bounds(rowels, clo, cup, hcol,
			 krs, kre,
			 &maxup, &maxdown,
			 j, rlo[row], rup[row], &ilow, &iup);
	  
	  
	  if (maxup < PRESOLVE_INF && maxup + tol < rlo[row]) {
	    /* there is an upper bound and it can't be reached */
	    prob->status_|= 1;
	    prob->messageHandler()->message(COIN_PRESOLVE_ROWINFEAS,
					    prob->messages())
					      <<row
					      <<rlo[row]
					      <<rup[row]
					      <<CoinMessageEol;
	    break;
	  } else if (-PRESOLVE_INF < maxdown && rup[row] < maxdown - tol) {
	    /* there is a lower bound and it can't be reached */
	    prob->status_|= 1;
	    prob->messageHandler()->message(COIN_PRESOLVE_ROWINFEAS,
					    prob->messages())
					      <<row
					      <<rlo[row]
					      <<rup[row]
					      <<CoinMessageEol;
	    break;
	  } else if (clo[j] <= ilow && iup <= cup[j]) {
	    
	    // both column bounds implied by the constraints of the problem
	    implied_free[j] = row;
	    infiniteUp[row]=-3;
	    //printf("column %d implied free by row %d hincol %d hinrow %d\n",
	    //   j,row,hincol[j],hinrow[row]);
	  }
	}
      }
    }
  }
  // implied_free[j] == hincol[j] && hincol[j] > 0 ==> j is implied free

  delete [] infiniteDown;
  delete [] infiniteUp;
  delete [] maxDown;
  delete [] maxUp;

  int isolated_row = -1;

  // first pick off the easy ones
  // note that this will only deal with columns that were originally
  // singleton; it will not deal with doubleton columns that become
  // singletons as a result of dropping rows.
  for (iLook=0;iLook<numberLook;iLook++) {
    int j=look[iLook];
    if (hincol[j] == 1 && implied_free[j] >=0) {
      CoinBigIndex kcs = mcstrt[j];
      int row = hrow[kcs];
      double coeffj = colels[kcs];

      CoinBigIndex krs = mrstrt[row];
      CoinBigIndex kre = krs + hinrow[row];


      // isolated rows are weird
      {
	int n = 0;
	for (CoinBigIndex k=krs; k<kre; ++k)
	  n += hincol[hcol[k]];
	if (n==hinrow[row]) {
	  isolated_row = row;
	  break;
	}
      }

      const bool nonzero_cost = (cost[j] != 0.0&&fabs(rup[row]-rlo[row])<=tol);

      double *save_costs = nonzero_cost ? new double[hinrow[row]] : NULL;

      {
	action *s = &actions[nactions++];
	      
	s->row = row;
	s->col = j;

	s->clo = clo[j];
	s->cup = cup[j];
	s->rlo = rlo[row];
	s->rup = rup[row];

	s->ninrow = hinrow[row];
	s->rowels = presolve_duparray(&rowels[krs], hinrow[row]);
	s->rowcols = presolve_duparray(&hcol[krs], hinrow[row]);
	s->costs = save_costs;
      }

      if (nonzero_cost) {
	double rhs = rlo[row];
	double costj = cost[j];

#if	DEBUG_PRESOLVE
	printf("FREE COSTS:  %g  ", costj);
#endif
	for (CoinBigIndex k=krs; k<kre; k++) {
	  int jcol = hcol[k];
	  save_costs[k-krs] = cost[jcol];

	  if (jcol != j) {
	    double coeff = rowels[k];

#if	DEBUG_PRESOLVE
	    printf("%g %g   ", cost[jcol], coeff/coeffj);
#endif
	    /*
	     * Similar to eliminating doubleton:
	     *   cost1 x = cost1 (c - b y) / a = (c cost1)/a - (b cost1)/a
	     *   cost[icoly] += cost[icolx] * (-coeff2 / coeff1);
	     */
	    cost[jcol] += costj * (-coeff / coeffj);
	  }
	}
#if	DEBUG_PRESOLVE
	printf("\n");

	/* similar to doubleton */
	printf("BIAS??????? %g %g %g %g\n",
	       costj * rhs / coeffj,
	       costj, rhs, coeffj);
#endif
	prob->change_bias(costj * rhs / coeffj);
	// ??
	cost[j] = 0.0;
      }

      /* remove the row from the columns in the row */
      for (CoinBigIndex k=krs; k<kre; k++) {
	int jcol=hcol[k];
	prob->addCol(jcol);
	presolve_delete_from_row(jcol, row, mcstrt, hincol, hrow, colels);
      }
      PRESOLVE_REMOVE_LINK(rlink, row);
      hinrow[row] = 0;

      // just to make things squeeky
      rlo[row] = 0.0;
      rup[row] = 0.0;

      PRESOLVE_REMOVE_LINK(clink, j);
      hincol[j] = 0;

      implied_free[j] = -1;	// probably unnecessary
    }
  }

  delete [] look2;
  if (nactions) {
#if	PRESOLVE_SUMMARY
    printf("NIMPLIED FREE:  %d\n", nactions);
#endif
    action *actions1 = new action[nactions];
    CoinDisjointCopyN(actions, nactions, actions1);
    next = new implied_free_action(nactions, actions1, next);
  } 
  delete [] actions;

  if (isolated_row != -1) {
    const CoinPresolveAction *nextX = isolated_constraint_action::presolve(prob, 
						isolated_row, next);
    if (nextX)
      next = nextX; // may fail
  }
  // try more complex ones
  if (fill_level) {
    next = subst_constraint_action::presolve(prob, implied_free, next,fill_level);
  }
  delete[]implied_free;

  return (next);
}



const char *implied_free_action::name() const
{
  return ("implied_free_action");
}

void implied_free_action::postsolve(CoinPostsolveMatrix *prob) const
{
  const action *const actions = actions_;
  const int nactions = nactions_;

  double *elementByColumn	= prob->colels_;
  int *hrow		= prob->hrow_;
  CoinBigIndex *columnStart		= prob->mcstrt_;
  int *numberInColumn		= prob->hincol_;
  int *link		= prob->link_;

  double *clo	= prob->clo_;
  double *cup	= prob->cup_;

  double *rlo	= prob->rlo_;
  double *rup	= prob->rup_;

  double *sol	= prob->sol_;

  double *rcosts	= prob->rcosts_;
  double *dcost		= prob->cost_;

  double *acts	= prob->acts_;
  double *rowduals = prob->rowduals_;

  //  const double ztoldj	= prob->ztoldj_;

  const double maxmin	= prob->maxmin_;

  char *cdone	= prob->cdone_;
  char *rdone	= prob->rdone_;
  CoinBigIndex free_list = prob->free_list_;

  for (const action *f = &actions[nactions-1]; actions<=f; f--) {

    int irow = f->row;
    int icol = f->col;
	  
    int ninrow = f->ninrow;
    const double *rowels = f->rowels;
    const int *rowcols = f->rowcols;
    const double *save_costs = f->costs;

    // put back coefficients in the row
    // this includes recreating the singleton column
    {
      for (int k = 0; k<ninrow; k++) {
	int jcol = rowcols[k];
	double coeff = rowels[k];

	if (save_costs) {
	  rcosts[jcol] += maxmin*(save_costs[k]-dcost[jcol]);
	  dcost[jcol] = save_costs[k];
	}
	{
	  CoinBigIndex kk = free_list;
	  free_list = link[free_list];

	  check_free_list(free_list);

	  link[kk] = columnStart[jcol];
	  columnStart[jcol] = kk;
	  elementByColumn[kk] = coeff;
	  hrow[kk] = irow;
	}

	if (jcol == icol) {
	  // initialize the singleton column
	  numberInColumn[jcol] = 1;
	  clo[icol] = f->clo;
	  cup[icol] = f->cup;

	  cdone[icol] = IMPLIED_FREE;
	} else {
	  numberInColumn[jcol]++;
	}
      }
      rdone[irow] = IMPLIED_FREE;

      rlo[irow] = f->rlo;
      rup[irow] = f->rup;
    }
    deleteAction( save_costs,double*);
    // coeff has now been initialized

    // compute solution
    {
      double act = 0.0;
      double coeff = 0.0;

      for (int k = 0; k<ninrow; k++)
	if (rowcols[k] == icol)
	  coeff = rowels[k];
	else {
	  int jcol = rowcols[k];
	  PRESOLVE_STMT(CoinBigIndex kk = presolve_find_row2(irow, columnStart[jcol], numberInColumn[jcol], hrow, link));
	  act += rowels[k] * sol[jcol];
	}
	    
      PRESOLVEASSERT(fabs(coeff) > ZTOLDP);
      double thisCost = maxmin*dcost[icol];
      double loActivity,upActivity;
      if (coeff>0) {
	loActivity = (rlo[irow]-act)/coeff;
	upActivity = (rup[irow]-act)/coeff;
      } else {
	loActivity = (rup[irow]-act)/coeff;
	upActivity = (rlo[irow]-act)/coeff;
      }
      loActivity = max(loActivity,clo[icol]);
      upActivity = min(upActivity,cup[icol]);
      int where; //0 in basis, -1 at lb, +1 at ub
      const double tolCheck	= 0.1*prob->ztolzb_;
      if (loActivity<clo[icol]+tolCheck/fabs(coeff)&&thisCost>=0.0)
	where=-1;
      else if (upActivity>cup[icol]-tolCheck/fabs(coeff)&&thisCost<0.0)
	where=1;
      else
	where =0;
      // But we may need to put in basis to stay dual feasible
      double possibleDual = thisCost/coeff;
      if (where) {
	double worst=  prob->ztoldj_;
	for (int k = 0; k<ninrow; k++) {
	  int jcol = rowcols[k];
	  if (jcol!=icol) {
	    CoinPrePostsolveMatrix::Status status = prob->getColumnStatus(jcol);
	    // can only trust basic
	    if (status==CoinPrePostsolveMatrix::basic) {
	      if (fabs(rcosts[jcol])>worst)
		worst=fabs(rcosts[jcol]);
	    } else if (sol[jcol]<clo[jcol]+ZTOLDP) {
	      if (-rcosts[jcol]>worst)
		worst=-rcosts[jcol];
	    } else if (sol[jcol]>cup[jcol]-ZTOLDP) {
	      if (rcosts[jcol]>worst)
		worst=rcosts[jcol];
	    } 
	  }
	}
	if (worst>prob->ztoldj_) {
	  // see if better if in basis
	  double worst2	= prob->ztoldj_;
	  for (int k = 0; k<ninrow; k++) {
	    int jcol = rowcols[k];
	    if (jcol!=icol) {
	      double coeff = rowels[k];
	      double newDj = rcosts[jcol]-possibleDual*coeff;
	      CoinPrePostsolveMatrix::Status status = prob->getColumnStatus(jcol);
	      // can only trust basic
	      if (status==CoinPrePostsolveMatrix::basic) {
		if (fabs(newDj)>worst2)
		  worst2=fabs(newDj);
	      } else if (sol[jcol]<clo[jcol]+ZTOLDP) {
		if (-newDj>worst2)
		  worst2=-newDj;
	      } else if (sol[jcol]>cup[jcol]-ZTOLDP) {
		if (newDj>worst2)
		  worst2=newDj;
	      } 
	    }
	  }
	  if (worst2<worst)
	    where=0; // put in basis
	}
      }
      if (!where) {
	// choose rowdual to make this col basic
	rowduals[irow] = possibleDual;
	if ((rlo[irow] < rup[irow] && rowduals[irow] < 0.0)
	    || rlo[irow]< -1.0e20) {
	  if (rlo[irow]<-1.0e20&&rowduals[irow]>ZTOLDP)
	    printf("IMP %g %g %g\n",rlo[irow],rup[irow],rowduals[irow]);
	  sol[icol] = (rup[irow] - act) / coeff;
	  assert (sol[icol]>=clo[icol]-1.0e-5&&sol[icol]<=cup[icol]+1.0e-5);
	  acts[irow] = rup[irow];
	  prob->setRowStatus(irow,CoinPrePostsolveMatrix::atUpperBound);
	} else {
	  sol[icol] = (rlo[irow] - act) / coeff;
	  assert (sol[icol]>=clo[icol]-1.0e-5&&sol[icol]<=cup[icol]+1.0e-5);
	  acts[irow] = rlo[irow];
	  prob->setRowStatus(irow,CoinPrePostsolveMatrix::atLowerBound);
	}
	prob->setColumnStatus(icol,CoinPrePostsolveMatrix::basic);
	for (int k = 0; k<ninrow; k++) {
	  int jcol = rowcols[k];
	  double coeff = rowels[k];
	  rcosts[jcol] -= possibleDual*coeff;
	}
	rcosts[icol] = 0.0;
      } else {
	rowduals[irow] = 0.0;
	rcosts[icol] = thisCost;
	prob->setRowStatus(irow,CoinPrePostsolveMatrix::basic);
	if (where<0) {
	  // to lb
	  prob->setColumnStatus(icol,CoinPrePostsolveMatrix::atLowerBound);
	  sol[icol]=clo[icol];
	} else {
	  // to ub
	  prob->setColumnStatus(icol,CoinPrePostsolveMatrix::atUpperBound);
	  sol[icol]=cup[icol];
	}
	acts[irow] = act + sol[icol]*coeff;
	assert (acts[irow]>=rlo[irow]-1.0e-5&&acts[irow]<=rup[irow]+1.0e-5);
      }
#if	DEBUG_PRESOLVE
      {
	double *colels	= prob->colels_;
	int *hrow	= prob->hrow_;
	const CoinBigIndex *mcstrt	= prob->mcstrt_;
	int *hincol	= prob->hincol_;
	for (int j = 0; j<ninrow; j++) {
	  int jcol = rowcols[j];
	  CoinBigIndex k = mcstrt[jcol];
	  int nx = hincol[jcol];
	  double dj = dcost[jcol];
	  for (int i=0; i<nx; ++i) {
	    int row = hrow[k];
	    double coeff = colels[k];
	    k = link[k];
	    dj -= rowduals[row] * coeff;
	    //printf("col jcol row %d coeff %g dual %g new dj %g\n",
	    // row,coeff,rowduals[row],dj);
	  }
	  if (fabs(dj-rcosts[jcol])>1.0e-3)
	    printf("changed\n");
	}
      }
#endif
    }
  }
  prob->free_list_ = free_list;
}
implied_free_action::~implied_free_action() 
{ 
  int i;
  for (i=0;i<nactions_;i++) {
    //delete [] actions_[i].rowcols; MS Visual C++ V6 can not compile
    //delete [] actions_[i].rowels; MS Visual C++ V6 can not compile
    deleteAction(actions_[i].rowcols,int *);
    deleteAction(actions_[i].rowels,int *);
    //delete [] actions_[i].costs; deleted earlier
  }
  // delete [] actions_; MS Visual C++ V6 can not compile
  deleteAction(actions_,action *);
}
