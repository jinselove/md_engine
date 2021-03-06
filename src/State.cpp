#include "Fix.h"
#include "Bounds.h"
#include "list_macro.h"
#include "AtomParams.h"
//#include "DataManager.h"
#include "WriteConfig.h"
#include "ReadConfig.h"
#include "PythonOperation.h"
#include "DataManager.h"
#include "DataSetUser.h"

/* State is where everything is sewn together. We set global options:
 *   - gpu cuda device data and options
 *   - atoms and groups
 *   - spatial options like grid/bound options, periodicity
 *   - fixes (local and shared)
 *   - neighbor lists
 *   - timestep
 *   - turn
 *   - cuts
 */

#include "State.h"

using std::cout;
using std::endl;
using namespace MD_ENGINE;

namespace py = boost::python;
State::State() : units(&dt) {
    groupTags["all"] = (unsigned int) 1;
    is2d = false;
    rCut = RCUT_INIT;
    padding = PADDING_INIT;
    turn = 0;
    maxIdExisting = -1;
    maxExclusions = 0;
    dangerousRebuilds = 0;
    periodicInterval = 50;
    shoutEvery = 5000;
    for (int i=0; i<3; i++) {
        periodic[i] = true;
    }
    bounds = Bounds(this);

    //! \todo It would be nice to set verbose true/false in Logging.h and use
    //!       it for mdMessage.
    verbose = true;
    readConfig = SHARED(ReadConfig) (new ReadConfig(this));
    atomParams = AtomParams(this);
    requiresCharges = false; //will be set to true if a fix needs it (like ewald sum).  Is max of fixes requiresCharges bool
    dataManager = DataManager(this);
    integUtil = IntegratorUtil(this);
    specialNeighborCoefs[0] = 0;
    specialNeighborCoefs[1] = 0;
    specialNeighborCoefs[2] = 0.5;
    rng_is_seeded = false;
    nPerRingPoly  = 1;
    exclusionMode = EXCLUSIONMODE::DISTANCE;

    nThreadPerAtom = 1;
    nThreadPerBlock = 256;

    tuneEvery = 1000000;


}


uint State::groupTagFromHandle(std::string handle) {
    if (groupTags.find(handle) == groupTags.end()) {
        std::cout << "Count not find group " << handle << ".  Quitting. " << std::endl;
        assert(groupTags.find(handle) != groupTags.end());
    }
    return groupTags[handle];
}

bool State::atomInGroup(Atom &a, std::string handle) {
    uint tag = groupTagFromHandle(handle);
    return a.groupTag & tag;
}

int State::addAtom(std::string handle, Vector pos, double q) {
    std::vector<std::string> &handles = atomParams.handles;
    auto it = find(handles.begin(), handles.end(), handle);
    assert(it != handles.end());
    int idx = it - handles.begin();//okay, so index in handles is type
    Atom a(pos, idx, -1, atomParams.masses[idx], q, &handles);
    bool added = addAtomDirect(a);
    if (added) {
        return atoms.back().id;
    }
    return -1;
}

bool State::addAtomDirect(Atom a) {
	//overwriting atom id if it's set to the default, -1
	if (a.id == -1) {
		if (idBuffer.size()) {
			a.id = idBuffer.back();
			idBuffer.pop_back();
		} else {
			maxIdExisting++;
			a.id = maxIdExisting;
		}
	} else {
        auto it = find(idBuffer.begin(), idBuffer.end(), a.id);
        if (it != idBuffer.end()) {
            idBuffer.erase(it);
        }
        //not going to worry about populating id buffer, because presumably you are reading in from a system with well-packed ids
        if (maxIdExisting < a.id) {
            maxIdExisting = a.id;
        }
    }
    while (idToIdx.size() <= a.id) {
        idToIdx.push_back(0);
    }
    idToIdx[a.id] = atoms.size();

    if (a.type >= atomParams.numTypes) {
        std::cout << "Bad atom type " << a.type << std::endl;
        return false;
    }
    if (a.mass == -1 or a.mass == 0) {
        a.mass = atomParams.masses[a.type];
    }
    if (is2d) {
        if (fabs(a.pos[2]) > 0.2) { //some noise value if you aren't applying fix 2d every turn.  Should add override
            std::cout << "adding atom with large z value in 2d simulation. Not adding atom" << std::endl;
            return false;
        }
        a.pos[2] = 0;
    }

    atoms.push_back(a);
    return true;
}

Atom &State::duplicateAtom(Atom a) {
	a.id = -1; //will assign id if id == -1
    addAtomDirect(a); 
    return atoms.back();
}

Atom &State::idToAtom(int id) {
    return atoms[idToIdx[id]];
}

int State::idToIdxPy(int id) {
    return idToIdx[id];
}

//constructor should be same
/*
bool State::addBond(Atom *a, Atom *b, double k, double rEq) {
    if (a == b ||
        !(a >= &(*atoms.begin()) && a < &(*atoms.end())) ||
        !(b >= &(*atoms.begin()) && b < &(*atoms.end()))) {
        return false;
    }
    int *ids = (int *) malloc(sizeof(int) * 2);
    ids[0] = a->id;
    ids[1] = b->id;
    bondAtomIds.push_back(ids);
    Bond bond(a, b, k, rEq);
    bonds.push_back(bond);
    changedBonds = true;
    return true;
}
*/


bool State::deleteAtom(Atom *a) {
    if (!(a >= &(*atoms.begin()) && a < &(*atoms.end()))) {
        return false;
    }
    for (Fix *f : fixes) {
        f->deleteAtom(a);
    }
    int id = a->id;
    if (id == maxIdExisting) {
        maxIdExisting--;
        //need to collapse maxIdExisting to first space (of more that n1) in ids in idBuffer
        //this relies on sort line below
        while (idBuffer.size() and maxIdExisting == idBuffer.back()) {
            idBuffer.pop_back();
            maxIdExisting--;
        }
    } else {
        idBuffer.push_back(id);
        sort(idBuffer.begin(), idBuffer.end());
    }
    int idx = a - &atoms[0];
    atoms.erase(atoms.begin()+idx, atoms.begin()+idx+1);
    refreshIdToIdx(); //hey, if deleting multiple atoms, this doesn't need to be done for every one
    return true;
}

bool State::deleteMolecule(Molecule &m) {
    int len = py::len(molecules);
    for (int i=0; i<len; i++) {
        py::extract<Molecule &> molecPy(molecules[i]);
        mdAssert(molecPy.check(), "Non-molecule found in list of molecules"); 
        Molecule &molec = molecPy;
        if (m == molec) {
            for (int id : m.ids) {
                Atom *a = &idToAtom(id);
                deleteAtom(a);
            }
            molecules.pop(i);
            return true;
        }
    }
    mdAssert(false, "Could not find molecule to delete");
    return false;
}

void State::createMolecule(std::vector<int> &ids) {
    for (int id : ids) {
        mdAssert(idToIdx[id] != -1, "Invalid atom id given for molecule");
    }
    molecules.append(Molecule(this, ids));
}

py::object State::createMoleculePy(py::list idsPy) {
    int len = py::len(idsPy);
    std::vector<int> ids(len);
    for (int i=0; i<len; i++) {
        py::extract<int> idPy(idsPy[i]);
        if (!idPy.check()) {
            mdAssert(idPy.check(), "Non-integer number given for atom id in molecule");
        }
        int id = idPy;
        ids[i] = id;
    }
    createMolecule(ids);
    return molecules[py::len(molecules)-1];
}



py::object State::duplicateMolecule(Molecule &molec, int n) {
    int molecsOrig = py::len(molecules);
    std::vector<int> oldIds;
    std::vector< std::vector<int> > newIdss;
    for (int id : molec.ids) {
        oldIds.push_back(id);
    }
    for (int i=0; i<n; i++) {
        std::vector<int> newIds(oldIds.size());
        for (int j=0; j<oldIds.size(); j++) {
            int oldId = oldIds[j];
            Atom &a = atoms[idToIdx[oldId]];
            Atom &dup = duplicateAtom(a);
            newIds[j] = dup.id;
        }
        newIdss.push_back(newIds);
    }

    for (Fix *fix : fixes) {
        fix->duplicateMolecule(oldIds, newIdss);
    }
    py::list newMolecs;
    for (std::vector<int> &newIds : newIdss) {
        createMolecule(newIds);
        newMolecs.append(molecules[py::len(molecules)-1]);
    }
    if (n>1) {
        return newMolecs;
    }
    return molecules[py::len(molecules)-1];


}

void unwrapMolec(State *state, int id, std::vector<int> &molecIds, std::unordered_map<int, std::vector<int> > &bondMap) {
    Vector myPos = state->idToAtom(id).pos;
    if (bondMap.find(id) != bondMap.end()) {
        std::vector<int> &myConnections = bondMap[id];
        for (int i=myConnections.size()-1; i>=0; i--) {
            auto it = find(molecIds.begin(), molecIds.end(), myConnections[i]);
            if (it != molecIds.end()) {
                int otherId = *it;
                molecIds.erase(it);
                Atom &other = state->idToAtom(otherId);
                //unwrap
                other.pos = myPos + state->bounds.minImage(other.pos - myPos);

                //and recurse
                unwrapMolec(state, otherId, molecIds, bondMap);



            }

        }
    }
}

void State::unwrapMolecules() {
    std::vector<int> allMolecIds;
    std::vector<Molecule *> molecs;
    int nMolec = py::len(molecules);
    for (int i=0; i<nMolec; i++) {
        py::extract<Molecule *> molecEx(molecules[i]);
        mdAssert(molecEx.check(), "Non-molecule found in molecules list");
        Molecule *molec = molecEx;
        allMolecIds.insert(allMolecIds.end(), molec->ids.begin(), molec->ids.end());
        molecs.push_back(molec);
    }
    if (molecs.size() == 0) {
        return;
    }
    std::unordered_map<int, std::vector<int> > bondMap;
    //we do unwrapping based on topology so that it unwraps property even if it spans more than half the simulation box
    //This may be slow, but writes are async

    for (Fix *f : fixes) {
        std::vector<BondVariant> *fixBonds = f->getBonds();
        if (fixBonds != nullptr) {
            for (BondVariant &bv : *fixBonds) {
                const Bond &b = boost::apply_visitor(bondDowncast(bv), bv);
                if (find(allMolecIds.begin(), allMolecIds.end(), b.ids[0]) != allMolecIds.end() and find(allMolecIds.begin(), allMolecIds.end(), b.ids[1]) != allMolecIds.end()) {
                    bondMap[b.ids[0]].push_back(b.ids[1]);
                    bondMap[b.ids[1]].push_back(b.ids[0]);
                }
            }
        }
    }
    //now we just do graph search & reset to first's reference frame for each molec
    for (Molecule *molec : molecs) {
        std::vector<int> ids = molec->ids;
        int idBegin = ids.back();
        ids.pop_back();
        unwrapMolec(this, idBegin, ids, bondMap);
        Vector com = molec->COM();
        Vector comNew = bounds.wrap(com);
        Vector diff = comNew - com;
        molec->translate(diff);
    }
}



//void State::updateIdxFromIdCache() {
//    idToIdx = vector<int>(maxIdExisting+1);
//    for (int i=0; i<atoms.size(); i++) {
//        idToIdx[atoms[i].id] = i;
 //   }
//}

//complete refresh of idToIdx map.  used to removing atoms
void State::refreshIdToIdx() {
    idToIdx = std::vector<int>(maxIdExisting+1, -1);
    for (int i=0; i<atoms.size(); i++) {
        idToIdx[atoms[i].id] = i;
    }
}
/*  use atomParams.addSpecies
int State::addSpecies(std::string handle, double mass) {
    int id = atomParams.addSpecies(handle, mass);
    if (id != -1) {
        for (Fix *f : fixes) {
            f->addSpecies(handle);
        }
    }
    return id;
}
*/
void State::setSpecialNeighborCoefs(float onetwo, float onethree, float onefour) {
    specialNeighborCoefs[0] = onetwo;
    specialNeighborCoefs[1] = onethree;
    specialNeighborCoefs[2] = onefour;
}

void State::setExclusionMode(std::string mode) {
    if (mode == "forcer") {
        exclusionMode = EXCLUSIONMODE::FORCER;
    } else if (mode == "distance") {
        exclusionMode = EXCLUSIONMODE::DISTANCE;
    } else {
        mdAssert(false, "Exclusion mode must be 'forcer' or 'distance'");
    }
}

template <typename T>
int getSharedIdx(std::vector<SHARED(T)> &list, SHARED(T) other) {
    for (unsigned int i=0; i<list.size(); i++) {
        if (list[i]->handle == other->handle) {
            return i;
        }
    }
    return -1;
}

template <typename T>
bool removeGeneric(std::vector<SHARED(T)> &list, std::vector<T *> *unshared, SHARED(T) other) {
    int idx = getSharedIdx<T>(list, other);
    if (idx == -1) {
        return false;
    }
    list.erase(list.begin()+idx, list.begin()+idx+1);
    if (unshared != (std::vector<T *> *) NULL) {
        unshared->erase(unshared->begin()+idx, unshared->begin()+idx+1);
    }
    return true;
}

template <typename T>
bool addGeneric(std::vector<SHARED(T)> &list, std::vector<T *> *unshared, SHARED(T) other) {
    int idx = getSharedIdx<T>(list, other);
    if (idx != -1) {
        return false;
    }
    bool added = false;
    for (int idx=0; idx<list.size(); idx++) {
        SHARED(T)  existing = list[idx];
        if (other->orderPreference < existing->orderPreference) {
            list.insert(list.begin() + idx, other);
            if (unshared != (std::vector<T *> *) NULL) {
                unshared->insert(unshared->begin() + idx, other.get());
            }
            added = true;
            break;

        }

    }
    if (not added) {
        list.insert(list.end(), other);
        if (unshared != (std::vector<T *> *) NULL) {
            unshared->insert(unshared->end(), other.get());
        }

    }
    return true;
}

bool State::activateWriteConfig(SHARED(WriteConfig) other) {
    return addGeneric<WriteConfig>(writeConfigs, (std::vector<WriteConfig *> *) NULL, other);
}
bool State::deactivateWriteConfig(SHARED(WriteConfig) other) {
    return removeGeneric<WriteConfig>(writeConfigs, (std::vector<WriteConfig *> *) NULL, other);
}

bool State::activatePythonOperation(SHARED(PythonOperation) other) {
    return addGeneric<PythonOperation>(pythonOperations, (std::vector<PythonOperation *> *) NULL, other);
}
bool State::deactivatePythonOperation(SHARED(PythonOperation) other) {
    return removeGeneric<PythonOperation>(pythonOperations, (std::vector<PythonOperation *> *) NULL, other);
}


bool State::activateFix(SHARED(Fix) other) {
    if (other->state != this) {
        std::cout << "Trying to add fix with handle " << other->handle
                  << ", but fix was initialized with a different State" << std::endl;
    }
    assert(other->state == this);
    return addGeneric<Fix>(fixesShr, &fixes, other);
}
bool State::deactivateFix(SHARED(Fix) other) {
    return removeGeneric<Fix>(fixesShr, &fixes, other);
}

float State::getMaxRCut() {
    float maxRCut = rCut;
    for (Fix *f : fixes) {
        std::vector<float> rCuts = f->getRCuts();
        for (float x : rCuts) {
            maxRCut = fmax(x, maxRCut);
        }
    }
    return maxRCut;
}



void State::initializeGrid() {
    double maxRCut = getMaxRCut();// ALSO PADDING PLS
    double gridDim = maxRCut + padding;
    gridGPU = GridGPU(this, gridDim, gridDim, gridDim, gridDim, exclusionMode);
    //testing
    //nThreadPerBlock = 64;
    //nThreadPerAtom = 4;
    //gridGPU.nThreadPerBlock(nThreadPerBlock);
    //gridGPU.nThreadPerAtom(nThreadPerAtom);
    //gridGPU.initArraysTune();

}

bool State::prepareForRun() {
    // fixes have already prepared by the time the integrator calls this prepare
    std::vector<float4> xs_vec, vs_vec, fs_vec;
    std::vector<uint> ids;
    std::vector<float> qs;

    requiresCharges = false;
    std::vector<bool> requireCharges = LISTMAP(Fix *, bool, fix, fixes, fix->requiresCharges);
    if (!requireCharges.empty()) {
        requiresCharges = *std::max_element(requireCharges.begin(), requireCharges.end());
    }
    requiresPostNVE_V = false;
    std::vector<bool> requirePostNVE_V = LISTMAP(Fix *, bool, fix, fixes, fix->requiresPostNVE_V);
    if (!requirePostNVE_V.empty()) {
        requiresPostNVE_V = *std::max_element(requirePostNVE_V.begin(), requirePostNVE_V.end());
    }


    int nAtoms = atoms.size();

    xs_vec.reserve(nAtoms);
    vs_vec.reserve(nAtoms);
    fs_vec.reserve(nAtoms);
    ids.reserve(nAtoms);
    qs.reserve(nAtoms);

    for (const auto &a : atoms) {
        xs_vec.push_back(make_float4(a.pos[0], a.pos[1], a.pos[2],
                                     *(float *)&a.type));
        vs_vec.push_back(make_float4(a.vel[0], a.vel[1], a.vel[2],
                                     1/a.mass));
        fs_vec.push_back(make_float4(a.force[0], a.force[1], a.force[2],
                                     *(float *)&a.groupTag));
        ids.push_back(a.id);
        qs.push_back(a.q);
    }
    //just setting host-side vectors
    //transfer happs in integrator->basicPrepare
    gpd.xs.set(xs_vec);
    gpd.vs.set(vs_vec);
    gpd.fs.set(fs_vec);
    gpd.ids.set(ids);
    if (requiresCharges) {
        gpd.qs.set(qs);
    }
    std::vector<Virial> virials(atoms.size(), Virial(0, 0, 0, 0, 0, 0));
    gpd.virials = GPUArrayGlobal<Virial>(nAtoms);
    gpd.virials.set(virials);
    //gpd.perParticleEng = GPUArrayGlobal<float>(nAtoms);
    // so... wanna keep ids tightly packed.  That's managed by program, not user
    std::vector<int> id_vec = LISTMAPREF(Atom, int, a, atoms, a.id);
    std::vector<int> idToIdxs_vec;
    int size = *std::max_element(id_vec.begin(), id_vec.end()) + 1;
    idToIdxs_vec.reserve(size);
    for (int i=0; i<size; i++) {
        idToIdxs_vec.push_back(-1);
    }
    for (int i=0; i<id_vec.size(); i++) {
        idToIdxs_vec[id_vec[i]] = i;
    }

    gpd.idToIdxsOnCopy = idToIdxs_vec;
    gpd.idToIdxs.set(idToIdxs_vec);
    bounds.handle2d();
    boundsGPU = bounds.makeGPU();
    float maxRCut = getMaxRCut();
    initializeGrid();

    gpd.xsBuffer = GPUArrayGlobal<float4>(nAtoms);
    gpd.vsBuffer = GPUArrayGlobal<float4>(nAtoms);
    gpd.fsBuffer = GPUArrayGlobal<float4>(nAtoms);
    gpd.idsBuffer = GPUArrayGlobal<uint>(nAtoms);

    return true;
}
void State::handleChargeOffloading() {
    for (Fix *f : fixes) {
        if (f->canOffloadChargePairCalc) {
            for (Fix *g : fixes) {
                if (g->canAcceptChargePairCalc and not g->hasAcceptedChargePairCalc) {
                    g->acceptChargePairCalc(f); 
                    f->hasOffloadedChargePairCalc = true;
                    g->hasAcceptedChargePairCalc = true;
                }
            }
        }
    }
}
void copyAsyncWithInstruc(State *state, std::function<void (int64_t )> cb, int64_t turn) {
    cudaStream_t stream;
    CUCHECK(cudaStreamCreate(&stream));
    state->gpd.xsBuffer.dataToHostAsync(stream);
    state->gpd.vsBuffer.dataToHostAsync(stream);
    state->gpd.fsBuffer.dataToHostAsync(stream);
    state->gpd.idsBuffer.dataToHostAsync(stream);
    CUCHECK(cudaStreamSynchronize(stream));
    std::vector<int> idToIdxsOnCopy = state->gpd.idToIdxsOnCopy;
    std::vector<float4> &xs = state->gpd.xsBuffer.h_data;
    std::vector<float4> &vs = state->gpd.vsBuffer.h_data;
    std::vector<float4> &fs = state->gpd.fsBuffer.h_data;
    std::vector<uint> &ids = state->gpd.idsBuffer.h_data;
    std::vector<Atom> &atoms = state->atoms;

    for (int i=0, ii=state->atoms.size(); i<ii; i++) {
        int id = ids[i];
        int idxWriteTo = idToIdxsOnCopy[id];
        atoms[idxWriteTo].pos = xs[i];
        atoms[idxWriteTo].vel = vs[i];
        atoms[idxWriteTo].force = fs[i];
    }
    cb(turn);
    CUCHECK(cudaStreamDestroy(stream));
}

bool State::asyncHostOperation(std::function<void (int64_t )> cb) {
    // buffers should already be allocated in prepareForRun, and num atoms
    // shouldn't have changed.
    gpd.xs.copyToDeviceArray((void *) gpd.xsBuffer.getDevData());
    gpd.vs.copyToDeviceArray((void *) gpd.vsBuffer.getDevData());
    gpd.fs.copyToDeviceArray((void *) gpd.fsBuffer.getDevData());
    gpd.ids.copyToDeviceArray((void *) gpd.idsBuffer.getDevData());
    bounds.set(boundsGPU);
    if (asyncData and asyncData->joinable()) {
        asyncData->join();
    }
    cudaDeviceSynchronize();
    //cout << "ASYNC IS NOT ASYNC" << endl;
    //copyAsyncWithInstruc(this, cb, turn);
    asyncData = SHARED(std::thread) ( new std::thread(copyAsyncWithInstruc, this, cb, turn));
    // okay, now launch a thread to start async copying, then wait for it to
    // finish, and set the cb on state (and maybe a flag if you can't set the
    // function lambda equal to null
    //ONE MIGHT ASK WHY I'M NOT JUST DOING THE CALLBACK FROM THE THREAD
    //THE ANSWER IS BECAUSE I WANT TO USE THIS FOR PYTHON BIZ, AND YOU CAN'T
    //CALL PYTHON FUNCTIONS FROM A THREAD AS FAR AS I KNOW
    //if thread exists, wait for it to finish
    //okay, now I have all of these buffers filled with data.  now let's just
    //launch a thread which does the writing.  At the end of each just (in main
    //iteration code, just have a join statement before any of the downloading
    //happens
}

bool State::downloadFromRun() {
    std::vector<float4> &xs = gpd.xs.h_data;
    std::vector<float4> &vs = gpd.vs.h_data;
    std::vector<float4> &fs = gpd.fs.h_data;
    std::vector<uint> &ids = gpd.ids.h_data;
    for (int i=0, ii=atoms.size(); i<ii; i++) {
        int id = ids[i];
        int idxWriteTo = gpd.idToIdxsOnCopy[id];
        atoms[idxWriteTo].pos = xs[i];
        atoms[idxWriteTo].vel = vs[i];
        atoms[idxWriteTo].force = fs[i];
    }
    bounds.set(boundsGPU);
    return true;
}

void State::finish() {
    for (Fix *f : fixes) {
        f->resetChargePairFlags();
    }
}

bool State::addToGroupPy(std::string handle, py::list toAdd) {//list of atom ids
    uint32_t tagBit = groupTagFromHandle(handle);  //if I remove asserts from this, could return things other than true, like if handle already exists
    int len = py::len(toAdd);
    for (int i=0; i<len; i++) {
        py::extract<int> idPy(toAdd[i]);
        mdAssert(idPy.check(), "Tried to add atom to group, but numerical value (atom id) was not given");
        int id = idPy;
        mdAssert(id>=0 and id <= idToIdx.size(), "Invalid atom found when trying to add to group");
        int idx = idToIdx[id];
        mdAssert(idx >= 0 and idx < atoms.size(), "Invalid atom found when trying to add to group");
        Atom *a = &atoms[idx];
        a->groupTag |= tagBit;
    }
    return true;

}

bool State::addToGroup(std::string handle, std::function<bool (Atom *)> testF) {
    int tagBit = addGroupTag(handle);
    for (Atom &a : atoms) {
        if (testF(&a)) {
            a.groupTag |= tagBit;
        }
    }
    return true;
}





bool State::deleteGroup(std::string handle) {
    uint tagBit = groupTagFromHandle(handle);
    assert(handle != "all");
    for (Atom &a : atoms) {
        a.groupTag &= ~tagBit;
    }
    removeGroupTag(handle);
    return true;
}

bool State::createGroup(std::string handle, py::list ids) {
    uint32_t res = addGroupTag(handle);
    if (!res) {
        std::cout << "Tried to create group " << handle
                  << " << that already exists" << std::endl;
        return false;
    }
    if (py::len(ids)) {
        addToGroupPy(handle, ids);
    }
    return true;
}

uint State::addGroupTag(std::string handle) {
    uint working = 0;
    assert(groupTags.find(handle) == groupTags.end());
    for (auto it=groupTags.begin(); it!=groupTags.end(); it++) {
        working |= it->second;
    }
    for (int i=0; i<32; i++) {
        uint potentialTag = 1 << i;
        if (! (working & potentialTag)) {
            groupTags[handle] = potentialTag;
            return potentialTag;
        }
    }
    return 0;
}

bool State::removeGroupTag(std::string handle) {
    auto it = groupTags.find(handle);
    assert(it != groupTags.end());
    groupTags.erase(it);
    return true;
}

std::vector<Atom *> State::selectGroup(std::string handle) {
    int tagBit = groupTagFromHandle(handle);
    return LISTMAPREFTEST(Atom, Atom *, a, atoms, &a, a.groupTag & tagBit);
}

std::vector<Atom> State::copyAtoms() {
    std::vector<Atom> save;
    save.reserve(atoms.size());
    for (Atom &a : atoms) {
        Atom copy = a;
        save.push_back(copy);
    }
    return save;
}


bool State::validAtom(Atom *a) {
    return a >= atoms.data() and a <= &atoms.back();
}

void State::deleteAtoms() {
    atoms.erase(atoms.begin(), atoms.end());
    idBuffer.erase(idBuffer.begin(), idBuffer.end());
    maxIdExisting = -1;
    idToIdx.erase(idToIdx.begin(), idToIdx.end());
    molecules = py::list();
}


void State::zeroVelocities() {
    for (Atom &a : atoms) {
        a.vel.zero();
    }
}

void State::destroy() {
    //if (bounds) {  //UNCOMMENT
    //  bounds->state = NULL;
    //}
    //UNCOMMENT
    //bounds = NULL;
    deleteAtoms();
}

std::mt19937 &State::getRNG() {
    if (!rng_is_seeded) {
        seedRNG();
    }
    return randomNumberGenerator;
}

void State::seedRNG(unsigned int seed) {
    if (seed == 0) {
        std::random_device randDev;
        randomNumberGenerator.seed(randDev());
    } else {
        randomNumberGenerator.seed(seed);
    }
    rng_is_seeded = true;
}

    //helper for reader funcs (LAMMPS reader)
Vector generateVector(State &s, py::list valsPy) {
    if (py::len(valsPy) == 3) {
        double vals[3];
        for (int i=0; i<3; i++) {
            py::extract<double> val(valsPy[i]);
            if (!val.check()) {
                printf("Tried to generate vector with invalid values\n");
                assert(val.check());
            }
            vals[i] = val;
        }
        return Vector(vals[0], vals[1], vals[2]);
    }
    return Vector();
}


bool State::preparePIMD(double temp) {
    if (nPerRingPoly > 1) {
        int nAtoms = atoms.size();          // this is current number of atoms in system
        int nTot   = nAtoms * nPerRingPoly; // this is the total number of beads for PIMD
        std::vector<Atom> RPatoms;          // new vector of atoms to replace current atoms
        RPatoms.reserve(nTot);              
        boost::python::list RPmolecules;    // new list of molecules to replace current list

        float betaP     = 1.0f / units.boltz / temp;        // Here simulation is run at betaP by default
        float omegaP    = (float) units.boltz * temp / units.hbar  ;
        float invP            = 1.0 / (float) nPerRingPoly;
        float twoPiInvP       = 2.0f * M_PI * invP;

        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // UPDATE ATOMS
        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        for (int i=0; i < nAtoms; i++) {
            Atom   ai  = atoms[i];
            int baseId = ai.id;
            baseId    *= nPerRingPoly;      // stride the existing id's by nPerRingPoly

            // prepare for position initialization
            // here we are sampling a set of normal-mode coordinates
            // from the free ring-polymer solution
            // those coordinates will then be used in a back transformation to obtain
            // a new set of coordinates with centroid at the initial position of the atoms
            std::vector<Vector> xsNM;       // vector for containing normal-mode coordinates
            xsNM.reserve(nPerRingPoly);
            xsNM.push_back(ai.pos* sqrtf( (float) nPerRingPoly));
            for (int k = 1; k < nPerRingPoly; k++) {
                float omegak = 2.0f * omegaP * sinf( k * twoPiInvP * 0.5);
                float sigmak = sqrtf((float) 1.0  / betaP / ai.mass / units.mvv_to_eng) / omegak; // sigma = sqrt(1/ beta_P * m *omegak^2)
                std::normal_distribution<float> distNM(0.0,sigmak);
                float xk, yk, zk;
                xk = distNM(randomNumberGenerator);
                yk = distNM(randomNumberGenerator);
                zk = distNM(randomNumberGenerator);
                xsNM.push_back(Vector(xk,yk,zk));
            }

            // prepare for velocity initialization
            std::normal_distribution<float> distVel(0.0,sqrtf( (float) 1.0 / betaP / ai.mass / units.mvv_to_eng));

            // fill in atom copies
            for (int k=0; k < nPerRingPoly; k++) {
                int RPid = baseId + k;          // get new ID based on position in ring polymer
                Atom aik = ai;                  // atom copy
                aik.id = RPid;                  // update id
                aik.setBeadPos(k+1,nPerRingPoly,xsNM);  // adjust position
                // resample velocity
                Vector vk;
                vk[0] = distVel(randomNumberGenerator); // vx
                vk[1] = distVel(randomNumberGenerator); // vy
                vk[2] = distVel(randomNumberGenerator); // vz
                aik.setVel(vk);                // adjust velocity
                RPatoms.push_back(aik);        // add atom to list
            }

        }
        // replace atoms with the new RPatoms list
        deleteAtoms();
        for (Atom &a : RPatoms) {
            addAtomDirect(a);
        }

        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // UPDATE MOLECULES
        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        int nMol = py::len(molecules);
        py::list molecsRP;
        // go over each molecule
        for (int i=0; i<nMol; i++) {
            py::extract<Molecule *> molecEx(molecules[i]);
            Molecule *moli = molecEx;
            std::vector<int> moliIds;   // vector for containing ids defining molecule
            // extract the ids for the molecule
            for (int id : moli->ids) {
                moliIds.push_back(id);
            }
            // iterate over beads/time slices and systematically increase IDs by timeslice
            int nAtmPerMol = moliIds.size();
            for (int k = 0; k < nPerRingPoly; k++) {
                std::vector<int> molikIds = moliIds;        // new ids for the molecule at kth timeslice
                for (int l = 0; l < nAtmPerMol; l++) {
                    molikIds[l] = moliIds[l]*nPerRingPoly +k;   // stride by nPerRingPoly and increase to time slice
                }
                molecsRP.append(Molecule(this,molikIds));       // add molecule
            }
        }
        // replace the list of molecules with the new list of molecsRP
        molecules = molecsRP;

        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // UPDATE FIXES
        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        for (Fix *fix : fixes) {
            fix->updateForPIMD(nPerRingPoly);
        }

        return true;
    }
    else {
        return false;
    }
}

BOOST_PYTHON_MEMBER_FUNCTION_OVERLOADS(State_seedRNG_overloads,State::seedRNG,0,1)

    void export_State() {
        py::class_<State,
            SHARED(State) >("State", py::init<>())
                .def("addAtom", &State::addAtom,
                        (py::arg("handle"),
                         py::arg("pos"),
                         py::arg("q")=0)
                    )
                .def_readonly("atoms", &State::atoms)
                .def_readonly("molecules", &State::molecules)
                .def("setPeriodic", &State::setPeriodic)
                .def("getPeriodic", &State::getPeriodic) //boost is grumpy about readwriting static arrays.  can readonly, but that's weird to only allow one w/ wrapper func for other.  doing wrapper funcs for both
                .def("deleteAtom", &State::deleteAtom)
                .def("deleteMolecule", &State::deleteMolecule)
                //.def("removeBond", &State::removeBond)

                .def("addToGroup", &State::addToGroupPy)
                .def("deleteGroup", &State::deleteGroup)
                .def("createGroup", &State::createGroup,
                        (py::arg("handle"),
                         py::arg("atoms") = py::list())
                    )
				.def("atomInGroup", &State::atomInGroup)
                .def("createMolecule", &State::createMoleculePy, (py::arg("ids")))
                .def("duplicateMolecule", &State::duplicateMolecule, (py::arg("molecule"), py::arg("n")=1))
                .def("selectGroup", &State::selectGroup)
                .def("copyAtoms", &State::copyAtoms)
                .def("idToIdx", &State::idToIdxPy)
                .def("setSpecialNeighborCoefs", &State::setSpecialNeighborCoefs)

                .def("activateFix", &State::activateFix)
                .def("deactivateFix", &State::deactivateFix)
                .def("activateWriteConfig", &State::activateWriteConfig)
                .def("deactivateWriteConfig", &State::deactivateWriteConfig)
                .def("activatePythonOperation", &State::activatePythonOperation)
                .def("deactivatePythonOperation", &State::deactivatePythonOperation)
                .def("zeroVelocities", &State::zeroVelocities)
                .def("destroy", &State::destroy)
                .def("seedRNG", &State::seedRNG, State_seedRNG_overloads())
                .def("preparePIMD", &State::preparePIMD)
                .def_readwrite("is2d", &State::is2d)
                .def_readwrite("turn", &State::turn)
                .def_readwrite("nThreadPerAtom", &State::nThreadPerAtom)
                .def_readwrite("nThreadPerBlock", &State::nThreadPerBlock)
                .def_readwrite("tuneEvery", &State::tuneEvery)
                .def_readwrite("periodicInterval", &State::periodicInterval)
                .def_readwrite("rCut", &State::rCut)
                .def_readwrite("nPerRingPoly", &State::nPerRingPoly)
                .def_readwrite("dt", &State::dt)
                .def_readwrite("padding", &State::padding)
                .def_readonly("groupTags", &State::groupTags)
                .def_readonly("dataManager", &State::dataManager)
                //shared ptrs
                .def_readwrite("bounds", &State::bounds)
                .def_readwrite("fixes", &State::fixesShr)
                .def_readwrite("atomParams", &State::atomParams)
                .def_readwrite("writeConfigs", &State::writeConfigs)
                .def_readonly("readConfig", &State::readConfig)
                .def_readwrite("shoutEvery", &State::shoutEvery)
                .def_readwrite("verbose", &State::verbose)
                .def_readonly("deviceManager", &State::devManager)
                .def_readonly("units", &State::units)
                //helper for reader funcs
                .def("Vector", &generateVector, (py::arg("vals")=py::list()))


                ;

    }


