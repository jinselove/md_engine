#include "FixChargePairDSF.h"
#include "cutils_func.h"
// #include <cmath>

//Pairwise force shifted damped Coulomb
//Christopher J. Fennel and J. Daniel Gezelter J. Chem. Phys (124), 234104 2006
// Eqn 19.
//force calculation:
//  F=q_i*q_j*[erf(alpha*r    )/r^2   +2*alpha/sqrt(Pi)*exp(-alpha^2*r^2    )/r
//	      -erf(alpha*r_cut)/rcut^2+2*alpha/sqrt(Pi)*exp(-alpha^2*r_cut^2)/r_cut]

//or F=q_i*q_j*[erf(alpha*r    )/r^2   +A*exp(-alpha^2*r^2    )/r- shift*r]
//with   A   = 2*alpha/sqrt(Pi)
//     shift = erf(alpha*r_cut)/r_cut^3+2*alpha/sqrt(Pi)*exp(-alpha^2*r_cut^2)/r_cut^2


//    compute_cu<<<NBLOCK(nAtoms), PERBLOCK>>>(nAtoms, gpd.xs(activeIdx), gpd.fs(activeIdx), neighborCounts, grid.neighborlist.data(), grid.perBlockArray.d_data.data(), gpd.qs(activeIdx), alpha, r_cut, A, shift, state->boundsGPU, state->devManager.prop.warpSize, 0.5);// state->devManager.prop.warpSize, sigmas.getDevData(), epsilons.getDevData(), numTypes, state->rCut, state->boundsGPU, oneFourStrength);
__global__ void compute_charge_pair_DSF_cu(int nAtoms, float4 *xs, float4 *fs, int *neighborCounts, uint *neighborlist, int *cumulSumMaxPerBlock, float *qs, float alpha, float rCut,float A, float shift, BoundsGPU bounds, int warpSize, float oneFourStrength) {

    float multipliers[4] = {1, 0, 0, oneFourStrength};
    int idx = GETIDX();
    if (idx < nAtoms) {
        float4 posWhole = xs[idx];
        float3 pos = make_float3(posWhole);

        float3 forceSum = make_float3(0, 0, 0);
        float qi = qs[idx];//tex2D<float>(qs, XIDX(idx, sizeof(float)), YIDX(idx, sizeof(float)));

        //printf("start, end %d %d\n", start, end);
        int baseIdx = baseNeighlistIdx<void>(cumulSumMaxPerBlock, warpSize);
        int numNeigh = neighborCounts[idx];
        for (int i=0; i<numNeigh; i++) {
            int nlistIdx = baseIdx + warpSize * i;
            uint otherIdxRaw = neighborlist[nlistIdx];
            uint neighDist = otherIdxRaw >> 30;
            uint otherIdx = otherIdxRaw & EXCL_MASK;
            float3 otherPos = make_float3(xs[otherIdx]);
            //then wrap and compute forces!
            float3 dr = bounds.minImage(pos - otherPos);
            float lenSqr = lengthSqr(dr);
            //   printf("dist is %f %f %f\n", dr.x, dr.y, dr.z);
            if (lenSqr < rCut*rCut) {
                float multiplier = multipliers[neighDist];
                float len=sqrtf(lenSqr);
                float qj = qs[otherIdx];

                float r2inv = 1.0f/lenSqr;
                float rinv = 1.0f/len;
                float forceScalar = qi*qj*(erfcf((alpha*len))*r2inv+A*exp(-alpha*alpha*lenSqr)*rinv-shift)*rinv * multiplier;

		
                float3 forceVec = dr * forceScalar;
                forceSum += forceVec;
            }

        }   
        fs[idx] += forceSum; //operator for float4 + float3

    }

}
FixChargePairDSF::FixChargePairDSF(SHARED(State) state_, string handle_, string groupHandle_) : FixCharge(state_, handle_, groupHandle_, chargePairDSF) {
   setParameters(0.25,9.0);
};

void FixChargePairDSF::setParameters(float alpha_,float r_cut_)
{
  alpha=alpha_;
  r_cut=r_cut_;
  A= 2.0/sqrt(M_PI)*alpha;
  shift=std::erfc(alpha*r_cut)/(r_cut*r_cut)+A*exp(-alpha*alpha*r_cut*r_cut)/(r_cut);
}

void FixChargePairDSF::compute() {
    int nAtoms = state->atoms.size();
    GPUData &gpd = state->gpd;
    GridGPU &grid = state->gridGPU;
    int activeIdx = gpd.activeIdx;
    int *neighborCounts = grid.perAtomArray.d_data.data();
    compute_charge_pair_DSF_cu<<<NBLOCK(nAtoms), PERBLOCK>>>(nAtoms, gpd.xs(activeIdx), gpd.fs(activeIdx), neighborCounts, grid.neighborlist.data(), grid.perBlockArray.d_data.data(), gpd.qs(activeIdx), alpha, r_cut, A, shift, state->boundsGPU, state->devManager.prop.warpSize, 0.5);// state->devManager.prop.warpSize, sigmas.getDevData(), epsilons.getDevData(), numTypes, state->rCut, state->boundsGPU, oneFourStrength);
  //  compute_charge_pair_DSF_cu<<<NBLOCK(nAtoms), PERBLOCK>>>(nAtoms, gpd.xs(activeIdx), gpd.fs(activeIdx), neighborIdxs, grid.neighborlist.tex, gpd.qs(activeIdx), alpha,r_cut, A,shift, state->boundsGPU, 0.5);


}


void export_FixChargePairDSF() {
    class_<FixChargePairDSF, SHARED(FixChargePairDSF), bases<FixCharge> > ("FixChargePairDSF", init<SHARED(State), string, string> (args("state", "handle", "groupHandle")))
        .def("setParameters", &FixChargePairDSF::setParameters, (python::arg("alpha"), python::arg("r_cut")))
        ;
}
