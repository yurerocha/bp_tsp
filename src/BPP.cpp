#include "../include/BPP.h"

BPP::BPP(const std::shared_ptr<Data> pData, IloEnv& rEnv)
    : mpData(pData), mMasterModel(rEnv), 
      mLambdas(rEnv, mpData->getNbItems(), 0, IloInfinity),
      mObj(rEnv), mPartitionConstr(rEnv), 
	  mBestIntObj(std::numeric_limits<double>::infinity()) {

	for (int i = 0; i < mpData->getNbItems(); i++) {
		std::ostringstream oss;
        oss << "y" << i;

		mLambdas[i].setName(oss.str().c_str());
		mObj += M * mLambdas[i];

		mPartitionConstr.add(mLambdas[i] == 1);
		
		mItems.push_back({i});
	}

	mMasterModel.add(mPartitionConstr);

	mMasterObj = IloMinimize(rEnv, mObj);
	mMasterModel.add(mMasterObj);

	mMaster = IloCplex(mMasterModel);

    // Disables CPLEX log
	mMaster.setOut(rEnv.getNullStream());
}

std::pair<int, int> BPP::solve(const Node& rNode) {
	updateBounds(rNode);

	// Solve restricted master problem
	mMaster.solve();

	std::pair<int, int> b = {-1, -1};
    if (!rNode.isRoot && isgeq(std::ceil(mMaster.getObjValue()), mBestIntObj)) {
		// Prune
    	return b;
    }

	// Build and solve the pricing problem
	IloEnv env;
	IloModel pricingModel(env);
	IloExpr sumPacked(env);
	IloBoolVarArray x(env, mpData->getNbItems());

	for (int i = 0; i < mpData->getNbItems(); ++i) {
		// std::ostringstream oss;
		// oss << "x" << i;

		// x[i].setName(oss.str().c_str());

		sumPacked += mpData->getItemWeight(i) * x[i];
	}
	addPricingConstrs(rNode, pricingModel, x);

	IloObjective pricingObj = IloMinimize(env);
	pricingModel.add(pricingObj);

	pricingModel.add(sumPacked <= mpData->getBinCapacity());

	// std::cout << "Start column generation" << std::endl;
	// Begin column generation
	while (mMaster.getStatus() == IloAlgorithm::Optimal) { 
		IloCplex pricingProblem(pricingModel);
		pricingProblem.setParam(IloCplex::Param::Threads, 1);
		// Disables CPLEX log
		pricingProblem.setOut(env.getNullStream());

		// Get the dual variables
		auto pi = getDuals(env);

        IloExpr sumPricing(env, 1);
		for (int i = 0; i < mpData->getNbItems(); ++i) {
			sumPricing -= pi[i] * x[i];
		}

        pricingObj.setExpr(sumPricing);

		pricingProblem.solve();

		if (pricingProblem.getStatus() == IloAlgorithm::Infeasible) {
			env.end();
            pricingProblem.end();
			return b;
		}

		// std::cout << "Pricing obj:" << pricingProblem.getObjValue() 
		// 		  << std::endl;
		if (isl(pricingProblem.getObjValue(), 0.0)) {
			// std::cout << "Reduced cost is equal to " 
			// 		  << pricingProblem.getObjValue() 
			// 		  << ", which is less than 0..." << std::endl;

			IloNumArray enteringCol(env, mpData->getNbItems());

			pricingProblem.getValues(enteringCol, x);

			insertColumn(enteringCol);

			// std::cout << "Solving the RMP again..." << std::endl;

			mMaster.solve();
			
			// pricingProblem.clear();
			// pricingProblem.end();
		} else {
			// std::cout << "No column with negative reduced costs found. "
			// 		  << "The current basis is optimal" << std::endl;
			// std::cout << "Final master problem: " << std::endl;
			// system("cat model.lp");
			// pricingProblem.clear();
			// pricingProblem.end();
			break;
		}
	} // End column generation

	env.end();

	return computeBranchingItems();
}

void BPP::updateBounds(const Node& rNode) {
	double ub = 0.0;

	for (int i = mpData->getNbItems(); i < mLambdas.getSize(); ++i) {
		// Restore the bounds for the following iterations
		mLambdas[i].setUB(IloInfinity);
		// Force s.first and s.second to be in separate bins
		for (auto s : rNode.sep) {
			if (mItems[i].contains(s.first) == true 
				&& mItems[i].contains(s.second) == true) {
				mLambdas[i].setUB(ub);
			}
		}
		// Force t.first and t.second to be together in the same bin
		for (auto t : rNode.tog) {
			if (mItems[i].contains(t.first) != mItems[i].contains(t.second)) {
				mLambdas[i].setUB(ub);
			}
		}
	}
}

void BPP::addPricingConstrs(const Node& rNode, 
							IloModel& rPricingModel,
                            IloBoolVarArray& x) {
	// Force s.first and s.second to be in separate bins
	for (auto s : rNode.sep) {
		rPricingModel.add(x[s.first] + x[s.second] <= 1);
	}
	// Force t.first and t.second to be together in the same bin
	for (auto t : rNode.tog) {
		rPricingModel.add(x[t.first] == x[t.second]);
	}
}

std::pair<int, int> BPP::computeBranchingItems() {
	std::vector<std::vector<double>> z(mpData->getNbItems(), 
                                std::vector<double>(mpData->getNbItems(), 0.0));
	double bestDelta = std::numeric_limits<double>::infinity();
	std::pair<int, int> b(-1, -1);
	for (int i = 0; i < mpData->getNbItems(); ++i) {
		for (int j = i + 1; j < mpData->getNbItems(); ++j) {
			// The trivial cases can be ignored as, for them, each bean contains
			// a single item
			for (int k = mpData->getNbItems(); k < mItems.size(); ++k) {
				if (mItems[k].contains(i) && mItems[k].contains(j)) {
					z[i][j] += mMaster.getValue(mLambdas[k]);
				}
			}

			double delta = std::abs(z[i][j] - 0.5);
			if (isl(delta, bestDelta)) {
				bestDelta = delta;
				b = {i, j};
			}
		}
	}

	// If the solution is integer, the best delta will have value 0.5, i.e.
	// |0 - 0.5| = |1 - 0.5| = 0.5
	// In such a case, stop branching
	if (iseq(bestDelta, 0.5)) { 
		if (isl(mMaster.getObjValue(), mBestIntObj)) {
		    mBestIntObj = mMaster.getObjValue();
		}
		return {-1, -1};
	}

	return b;
}

IloNumArray BPP::getDuals(IloEnv& rEnv) const {
    IloNumArray pi(rEnv, mpData->getNbItems());
    mMaster.getDuals(pi, mPartitionConstr);

    return pi;
}

void BPP::insertColumn(IloNumArray& rCol) {
    std::unordered_set<int> newItemsCol;
    // std::cout << std::endl << "Entering column:" << std::endl;
    for (int i = 0; i < mpData->getNbItems(); ++i) {
        if (isg(rCol[i], 0.5)) {
            newItemsCol.insert(i);
        }
		// std::cout << rCol[i] << " ";
    }
	// std::cout << std::endl;
    mItems.push_back(newItemsCol);

    // Add the column to the master problem
    // (the cost of the new variable is always 1)
    std::ostringstream oss;
    oss << "y" << getNbLambda() + 1;

    // Essa linha é bruxaria pura; adiciona a variável na fo com custo 1 e já 
    // adiciona a restrição de partição
    IloNumVar newLambda(mMasterObj(1) + mPartitionConstr(rCol), 0, IloInfinity);
    newLambda.setName(oss.str().c_str());

    mLambdas.add(newLambda);
}

void BPP::printSol() const {
    for (int j = 0; j < mLambdas.getSize(); ++j) {
        std::cout << mMaster.getValue(mLambdas[j]) << " ";
    }
    std::cout << std::endl;
}

void BPP::printBins() const {
    for (int j = 0; j < mLambdas.getSize(); ++j) {
        if (!iseq(mMaster.getValue(mLambdas[j]), 0.0)) {
            std::cout << "Bin " << j << ": ";
            for (auto v : mItems[j]) {
                std::cout << v << " ";
            }
            std::cout << std::endl;
        }
    }
}