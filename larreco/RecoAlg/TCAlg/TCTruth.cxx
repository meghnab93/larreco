#include "larcorealg/Geometry/GeometryCore.h"
#include "larcoreobj/SimpleTypesAndConstants/geo_types.h"
#include "lardataobj/RecoBase/Hit.h"
#include "larreco/RecoAlg/TCAlg/TCVertex.h"
#include "larreco/RecoAlg/TCAlg/TCTruth.h"
#include "larreco/RecoAlg/TCAlg/Utils.h"
#include "nusimdata/SimulationBase/MCParticle.h"

#include "messagefacility/MessageLogger/MessageLogger.h"

#include <algorithm>
#include <bitset>
#include <iomanip>
#include <iostream>
#include <limits.h>
#include <stdlib.h>
#include <string>
#include <utility>

namespace tca {

  //////////////////////////////////////////
  void TruthMatcher::Initialize()
  {
     // Initialize the variables used to calculate Efficiency * Purity (aka EP) for matching to truth
    EPCnts.fill(0);
    TSums.fill(0.0);
    EPTSums.fill(0.0);
    TruVxCounts.fill(0);
    nBadT = 0;
  } // Initialize

  //////////////////////////////////////////
  void TruthMatcher::MatchTruth()
  {
    // Match trajectories, PFParticles, etc to the MC truth matched hits then
    // calculate reconstruction efficiency and purity. This function should only be
    // called once per event after reconstruction has been done in all slices

    // check for a serious error
    if(!evt.mcpHandle) return;
    // and no MCParticles
    if((*evt.mcpHandle).empty()) return;
    if(slices.empty()) return;
    if(evt.allHitsMCPIndex.size() != (*evt.allHits).size()) return;

    MatchTAndSum();
    MatchPAndSum();

  } // MatchTruth

  ////////////////////////////////////////////////
  void TruthMatcher::MatchTAndSum()
  {
    // match tjs in all tpcs that were reconstructed and sum
    
    // create a list of TPCs that were reconstructed
    std::vector<unsigned int> tpcList;
    for(auto& slc : slices) {
      unsigned int tpc = slc.TPCID.TPC;
      if(std::find(tpcList.end(), tpcList.end(), tpc) == tpcList.end()) tpcList.push_back(tpc);
    } // slc
    if(tpcList.empty()) return;

    // Hit -> T unique ID in all slices
    std::vector<int> inTUID((*evt.allHits).size(), 0);
    for(auto& slc : slices) {
      if(std::find(tpcList.begin(), tpcList.end(), slc.TPCID.TPC) == tpcList.end()) continue;
      for(auto& slh : slc.slHits) if(slh.InTraj > 0) {
        auto& tj = slc.tjs[slh.InTraj - 1];
        inTUID[slh.allHitsIndex] = tj.UID;
      }
    } // slc
    
    for(const geo::TPCID& tpcid : tcc.geom->IterateTPCIDs()) {
      // ignore protoDUNE dummy TPCs
      if(tcc.geom->TPC(tpcid).DriftDistance() < 25.0) continue;
      unsigned int tpc = tpcid.TPC;
      if(std::find(tpcList.begin(), tpcList.end(), tpc) == tpcList.end()) continue;
      // iterate over planes
      for(unsigned short plane = 0; plane < tcc.geom->Nplanes(); ++plane) {
        // form a list of MCParticles in this TPC and plane and the hit count
        std::vector<std::pair<unsigned int, float>> mCnt;
        // and lists of tj UIDs that use these hits
        std::vector<std::vector<std::pair<int, float>>> mtCnt;
        for(unsigned int iht = 0; iht < (*evt.allHits).size(); ++iht) {
          // require that it is MC-matched
          if(evt.allHitsMCPIndex[iht] == UINT_MAX) continue;
          // require that it resides in this tpc and plane
          auto& hit = (*evt.allHits)[iht];
          if(hit.WireID().TPC != tpc) continue;
          if(hit.WireID().Plane != plane) continue;
          unsigned int mcpi = evt.allHitsMCPIndex[iht];
          // find the mCnt entry
          unsigned short indx = 0;
          for(indx = 0; indx < mCnt.size(); ++indx) if(mcpi == mCnt[indx].first) break;
          if(indx == mCnt.size()) {
            mCnt.push_back(std::make_pair(mcpi, 0));
            mtCnt.resize(mCnt.size());
          }
          ++mCnt[indx].second;
          // see if it is used in a tj
          if(inTUID[iht] <= 0) continue;
          // find the mtCnt entry
          unsigned short tindx = 0;
          for(tindx = 0; tindx < mtCnt[indx].size(); ++tindx) if(mtCnt[indx][tindx].first == inTUID[iht]) break;
          if(tindx == mtCnt[indx].size()) mtCnt[indx].push_back(std::make_pair(inTUID[iht], 0));
          ++mtCnt[indx][tindx].second;
        } // iht
        if(mCnt.empty()) continue;
        for(unsigned short indx = 0; indx < mCnt.size(); ++indx) {
          // require at least 3 hits per plane to reconstruct
          if(mCnt[indx].second < 3) continue;
          // get a reference to the MCParticle and ensure that it is one we want to track
          auto& mcp = (*evt.mcpHandle)[mCnt[indx].first];
          float TMeV = 1000 * (mcp.E() - mcp.Mass());
          unsigned short pdgIndex = PDGCodeIndex(mcp.PdgCode());
          if(pdgIndex > 4) continue;
          TSums[pdgIndex] += TMeV;
          ++EPCnts[pdgIndex];
          int pdg = abs(mcp.PdgCode());
          // find the tj with the highest match count
          std::pair<int, float> big = std::make_pair(0, 0);
          for(unsigned short tindx = 0; tindx < mtCnt[indx].size(); ++tindx) {
            if(mtCnt[indx][tindx].second > big.second) big = mtCnt[indx][tindx];
          } // tindx
          if(big.first == 0) continue;
          auto slcIndex = GetSliceIndex("T", big.first);
          if(slcIndex.first == USHRT_MAX) continue;
          auto& slc = slices[slcIndex.first];
          auto& tj = slc.tjs[slcIndex.second];
          float npwc = NumPtsWithCharge(slc, tj, false);
          float eff = big.second / mCnt[indx].second;
          float pur = big.second / npwc;
          tj.EffPur = eff * pur;
          tj.mcpIndex = mCnt[indx].first;
          EPTSums[pdgIndex] += TMeV * tj.EffPur;
          // print BadEP ignoring electrons
          if(tj.EffPur < tcc.matchTruth[2] && (float)mCnt[indx].second > tcc.matchTruth[3] && pdgIndex > 0) {
            ++nBadT;
            std::string particleName = "Other";
            if(pdg == 11) particleName = "Electron";
            if(pdg == 22) particleName = "Photon";
            if(pdg == 13) particleName = "Muon";
            if(pdg == 211) particleName = "Pion";
            if(pdg == 321) particleName = "Kaon";
            if(pdg == 2212) particleName = "Proton";
            mf::LogVerbatim myprt("TC");
            myprt<<"BadT"<<tj.ID<<" TU"<<tj.UID<<" tpc "<<tpc;
            myprt<<" pln "<<plane<<" slice index "<<slcIndex.first;
            myprt<<" -> mcp "<<tj.mcpIndex<<" with mCnt = "<<(int)mCnt[indx].second;
            myprt<<" "<<particleName<<" T = "<<(int)TMeV<<" MeV";
            myprt<<" EP "<<std::fixed<<std::setprecision(2)<<tj.EffPur;
            // print the first and last hit
            unsigned int firstHit = UINT_MAX;
            unsigned int lastHit = 0;
            for(unsigned int iht = 0; iht < (*evt.allHits).size(); ++iht) {
              // require that it is matched to this MCP
              if(evt.allHitsMCPIndex[iht] != mCnt[indx].first) continue;
              // require that it resides in this tpc and plane
              auto& hit = (*evt.allHits)[iht];
              if(hit.WireID().TPC != tpc) continue;
              if(hit.WireID().Plane != plane) continue;
              if(firstHit == UINT_MAX) firstHit = iht;
              lastHit = iht;
            } // iht
            auto& fhit = (*evt.allHits)[firstHit];
            myprt<<" Hit range "<<fhit.WireID().Wire<<":"<<(int)fhit.PeakTime();
            auto& lhit = (*evt.allHits)[lastHit];
            myprt<<" - "<<lhit.WireID().Wire<<":"<<(int)lhit.PeakTime();
          } // Poor EP
        } // indx
      } // plane
    } // tpcid
  } // MatchTAndSum


  ////////////////////////////////////////////////
  void TruthMatcher::MatchPAndSum()
  {
    // match pfps in all tpcs that were reconstructed and sum. This is similar to MatchTAndSum
    // except there is no iteration over planes
    
    // create a list of TPCs that were reconstructed
    std::vector<unsigned int> tpcList;
    for(auto& slc : slices) {
      unsigned int tpc = slc.TPCID.TPC;
      if(std::find(tpcList.end(), tpcList.end(), tpc) == tpcList.end()) tpcList.push_back(tpc);
    } // slc
    if(tpcList.empty()) return;

    // Hit -> P unique ID in all slices
    std::vector<int> inPUID((*evt.allHits).size(), 0);
    // No sense trying to match to pfps if none exist
    bool matchPFPs = false;
    for(auto& slc : slices) {
      if(std::find(tpcList.begin(), tpcList.end(), slc.TPCID.TPC) == tpcList.end()) continue;
      // need to iterate through all TPs to get the tj -> TP -> hit -> PFP assn
      for(auto& tj : slc.tjs) {
        if(!slc.pfps.empty()) matchPFPs = true;
        if(tj.AlgMod[kKilled]) continue;
        // no sense looking if it wasn't 3D-matched
        if(!tj.AlgMod[kMat3D]) continue;
        for(auto& tp : tj.Pts) {
          if(tp.Chg <= 0) continue;
          if(tp.InPFP <= 0) continue;
          auto& pfp = slc.pfps[tp.InPFP - 1];
          for(unsigned short ii = 0; ii < tp.Hits.size(); ++ii) {
            if(!tp.UseHit[ii]) continue;
            unsigned int ahi = slc.slHits[tp.Hits[ii]].allHitsIndex;
            inPUID[ahi] = pfp.UID;
          } // ii
        } // tp
      } // tj
    } // slc
    if(!matchPFPs) return;
    
    // iterate over tpcs
    for(const geo::TPCID& tpcid : tcc.geom->IterateTPCIDs()) {
      // ignore protoDUNE dummy TPCs
      if(tcc.geom->TPC(tpcid).DriftDistance() < 25.0) continue;
      unsigned int tpc = tpcid.TPC;
      if(std::find(tpcList.begin(), tpcList.end(), tpc) == tpcList.end()) continue;
      // form a list of MCParticles in this TPC and the hit count
      std::vector<std::pair<unsigned int, float>> mCnt;
      // and lists of pfp UIDs that use these hits
      std::vector<std::vector<std::pair<int, float>>> mpCnt;
      for(unsigned int iht = 0; iht < (*evt.allHits).size(); ++iht) {
        // require that it is MC-matched
        if(evt.allHitsMCPIndex[iht] == UINT_MAX) continue;
        // require that it resides in this tpc and plane
        auto& hit = (*evt.allHits)[iht];
        if(hit.WireID().TPC != tpc) continue;
        unsigned int mcpi = evt.allHitsMCPIndex[iht];
        // find the mCnt entry
        unsigned short indx = 0;
        for(indx = 0; indx < mCnt.size(); ++indx) if(mcpi == mCnt[indx].first) break;
        if(indx == mCnt.size()) {
          mCnt.push_back(std::make_pair(mcpi, 0));
          mpCnt.resize(mCnt.size());
        }
        ++mCnt[indx].second;
        // see if it is used in a pfp
        if(inPUID[iht] <= 0) continue;
        // find the mpCnt entry
        unsigned short pindx = 0;
        for(pindx = 0; pindx < mpCnt[indx].size(); ++pindx) if(mpCnt[indx][pindx].first == inPUID[iht]) break;
        if(pindx == mpCnt[indx].size()) mpCnt[indx].push_back(std::make_pair(inPUID[iht], 0));
        ++mpCnt[indx][pindx].second;
      } // iht
      if(mCnt.empty()) continue;
      for(unsigned short indx = 0; indx < mCnt.size(); ++indx) {
        // require at least 3 hits per plane x 2 planes to reconstruct
        if(mCnt[indx].second < 6) continue;
        // get a reference to the MCParticle and ensure that it is one we want to track
        auto& mcp = (*evt.mcpHandle)[mCnt[indx].first];
        float TMeV = 1000 * (mcp.E() - mcp.Mass());
        unsigned short pdgIndex = PDGCodeIndex(mcp.PdgCode());
        if(pdgIndex > 4) continue;
        ++MCP_Cnt;
        MCP_TSum += TMeV;
        int pdg = abs(mcp.PdgCode());
        // find the tj with the highest match count
        std::pair<int, float> big = std::make_pair(0, 0);
        for(unsigned short pindx = 0; pindx < mpCnt[indx].size(); ++pindx) {
          if(mpCnt[indx][pindx].second > big.second) big = mpCnt[indx][pindx];
        } // pindx
        if(big.first == 0) continue;
        auto slcIndex = GetSliceIndex("P", big.first);
        if(slcIndex.first == USHRT_MAX) continue;
        auto& slc = slices[slcIndex.first];
        auto& pfp = slc.pfps[slcIndex.second];
        // assume that all IsBad points have been removed
        float npwc = pfp.TP3Ds.size();
        float eff = big.second / mCnt[indx].second;
        float pur = big.second / npwc;
        pfp.EffPur = eff * pur;
        pfp.mcpIndex = mCnt[indx].first;
        EPTSums[pdgIndex] += TMeV * pfp.EffPur;
        // print BadEP ignoring electrons
        if(pfp.EffPur < 0.8 && pdgIndex > 0) {
          ++nBadP;
          std::string particleName = "Other";
          if(pdg == 11) particleName = "Electron";
          if(pdg == 22) particleName = "Photon";
          if(pdg == 13) particleName = "Muon";
          if(pdg == 211) particleName = "Pion";
          if(pdg == 321) particleName = "Kaon";
          if(pdg == 2212) particleName = "Proton";
          mf::LogVerbatim myprt("TC");
          myprt<<"BadP"<<pfp.ID<<" PU"<<pfp.UID<<" tpc "<<tpc;
          myprt<<" slice index "<<slcIndex.first;
          myprt<<" -> mcp "<<pfp.mcpIndex<<" with mCnt = "<<(int)mCnt[indx].second;
          myprt<<" "<<particleName<<" T = "<<(int)TMeV<<" MeV";
          myprt<<" EP "<<std::fixed<<std::setprecision(2)<<pfp.EffPur;
        } // Poor EP
      } // indx
    } // tpcid

  } // MatchPAndSum

////////////////////////////////////////////////
  void TruthMatcher::PrintResults(int eventNum) const
  {
    // Print performance metrics for each selected event

    mf::LogVerbatim myprt("TC");
    myprt<<"Evt "<<eventNum;
    float sum = 0;
    float sumt = 0;
    for(unsigned short pdgIndex = 0; pdgIndex < TSums.size(); ++pdgIndex) {
      if(TSums[pdgIndex] == 0) continue;
      if(pdgIndex == 0) myprt<<" El";
      if(pdgIndex == 1) myprt<<" Mu";
      if(pdgIndex == 2) myprt<<" Pi";
      if(pdgIndex == 3) myprt<<" K";
      if(pdgIndex == 4) myprt<<" P";
      float ave = EPTSums[pdgIndex] / (float)TSums[pdgIndex];
      myprt<<" "<<std::fixed<<std::setprecision(2)<<ave;
//      myprt<<" "<<EPCnts[pdgIndex];
      if(pdgIndex > 0) {
        sum  += TSums[pdgIndex];
        sumt += EPTSums[pdgIndex];
      }
    } // pdgIndex
    if(sum > 0) myprt<<" MuPiKP "<<std::fixed<<std::setprecision(2)<<sumt / sum;
    myprt<<" nBadT "<<(int)nBadT;
    if(MCP_TSum > 0) {
      // PFParticle statistics
      float ep = MCP_EPTSum / MCP_TSum;
      myprt<<" MCP cnt "<<(int)MCP_Cnt<<" PFP EP "<<std::fixed<<std::setprecision(2)<<ep;
    }
    if(Prim_TSum > 0) {
      float ep = Prim_EPTSum / Prim_TSum;
      myprt<<" PrimPFP "<<std::fixed<<std::setprecision(2)<<ep;
    }
    if(TruVxCounts[1] > 0) {
      // True vertex is reconstructable
      float frac = (float)TruVxCounts[2] / (float)TruVxCounts[1];
      myprt<<" NuVx correct "<<std::fixed<<std::setprecision(2)<<frac;
    }
    myprt<<" nBadP "<<(int)nBadP;

  } // PrintResults

/* This code was used to develop the TMVA showerParentReader. The MakeCheatShower function needs
   to be re-written if this function is used in the future
   //////////////////////////////////////////
   void TruthMatcher::StudyShowerParents(TCSlice& slc, HistStuff& hist)
   {
   // study characteristics of shower parent pfps. This code is adapted from TCShower FindParent
   if(slc.pfps.empty()) return;
   if(slc.mcpList.empty()) return;

   // Look for truth pfp primary electron
   Point3_t primVx {{-666.0, -666.0, -666.0}};
   // the primary should be the first one in the list as selected in GetHitCollection
   auto& primMCP = slc.mcpList[0];
   primVx[0] = primMCP->Vx();
   primVx[1] = primMCP->Vy();
   primVx[2] = primMCP->Vz();
   geo::Vector_t posOffsets;
   auto const* SCE = lar::providerFrom<spacecharge::SpaceChargeService>();
   posOffsets = SCE->GetPosOffsets({primVx[0], primVx[1], primVx[2]});
   posOffsets.SetX(-posOffsets.X());
   primVx[0] += posOffsets.X();
   primVx[1] += posOffsets.Y();
   primVx[2] += posOffsets.Z();
   geo::TPCID inTPCID;
   // ignore if the primary isn't inside a TPC
   if(!InsideTPC(primVx, inTPCID)) return;
   // or if it is inside the wrong tpc
   if(inTPCID != slc.TPCID) return;

   std::string fcnLabel = "SSP";
   // Create a truth shower for each primary electron
   art::ServiceHandle<cheat::ParticleInventoryService const> pi_serv;
   MCParticleListUtils mcpu{slc};
   for(unsigned int part = 0; part < slc.mcpList.size(); ++part) {
   auto& mcp = slc.mcpList[part];
   // require electron or photon
   if(abs(mcp->PdgCode()) != 11 && abs(mcp->PdgCode()) != 111) continue;
   int eveID = pi_serv->ParticleList().EveId(mcp->TrackId());
   // require that it is primary
   if(mcp->TrackId() != eveID) continue;
   int truPFP = 0;
   auto ss3 = mcpu.MakeCheatShower(slc, part, primVx, truPFP);
   if(ss3.ID == 0) continue;
   if(truPFP == 0) continue;
   if(!StoreShower(fcnLabel, slc, ss3)) {
   std::cout<<"Failed to store 3S"<<ss3.ID<<"\n";
   break;
   } // store failed
   // now fill the TTree
   float ss3Energy = ShowerEnergy(ss3);
   for(auto& pfp : slc.pfps) {
   if(pfp.TPCID != ss3.TPCID) continue;
   // ignore neutrinos
   if(pfp.PDGCode == 12 || pfp.PDGCode == 14) continue;
   // ignore shower pfps
   if(pfp.PDGCode == 1111) continue;
   float pfpEnergy = 0;
   float minEnergy = 1E6;
   for(auto tid : pfp.TjIDs) {
   auto& tj = slc.tjs[tid - 1];
   float energy = ChgToMeV(tj.TotChg);
   pfpEnergy += energy;
   if(energy < minEnergy) minEnergy = energy;
   }
   pfpEnergy -= minEnergy;
   pfpEnergy /= (float)(pfp.TjIDs.size() - 1);
   // find the end that is farthest away
   unsigned short pEnd = FarEnd(slc, pfp, ss3.ChgPos);
   auto pToS = PointDirection(pfp.XYZ[pEnd], ss3.ChgPos);
   // take the absolute value in case the shower direction isn't well known
   float costh1 = std::abs(DotProd(pToS, ss3.Dir));
   float costh2 = DotProd(pToS, pfp.Dir[pEnd]);
   // distance^2 between the pfp end and the shower start, charge center, and shower end
   float distToStart2 = PosSep2(pfp.XYZ[pEnd], ss3.Start);
   float distToChgPos2 = PosSep2(pfp.XYZ[pEnd], ss3.ChgPos);
   float distToEnd2 = PosSep2(pfp.XYZ[pEnd], ss3.End);
   //        mf::LogVerbatim("TC")<<" 3S"<<ss3.ID<<" P"<<pfp.ID<<"_"<<pEnd<<" distToStart "<<sqrt(distToStart2)<<" distToChgPos "<<sqrt(distToChgPos2)<<" distToEnd "<<sqrt(distToEnd2);
   // find the end of the shower closest to the pfp
   unsigned short shEnd = 0;
   if(distToEnd2 < distToStart2) shEnd = 1;
   if(shEnd == 0 && distToChgPos2 < distToStart2) continue;
   if(shEnd == 1 && distToChgPos2 < distToEnd2) continue;
   //      mf::LogVerbatim("TC")<<" 3S"<<ss3.ID<<"_"<<shEnd<<" P"<<pfp.ID<<"_"<<pEnd<<" costh1 "<<costh1;
   Point2_t alongTrans;
   // find the longitudinal and transverse components of the pfp start point relative to the
   // shower center
   FindAlongTrans(ss3.ChgPos, ss3.Dir, pfp.XYZ[pEnd], alongTrans);
   //      mf::LogVerbatim("TC")<<"   alongTrans "<<alongTrans[0]<<" "<<alongTrans[1];
   hist.fSep = sqrt(distToChgPos2);
   hist.fShEnergy = ss3Energy;
   hist.fPfpEnergy = pfpEnergy;
   hist.fPfpLen = PosSep(pfp.XYZ[0], pfp.XYZ[1]);
   hist.fMCSMom = MCSMom(slc, pfp.TjIDs);
   hist.fDang1 = acos(costh1);
   hist.fDang2 = acos(costh2);
   hist.fChgFrac = 0;
   float chgFrac = 0;
   float totSep = 0;
   // find the charge fraction btw the pfp start and the point that is
   // half the distance to the charge center in each plane
   for(unsigned short plane = 0; plane < slc.nPlanes; ++plane) {
   CTP_t inCTP = EncodeCTP(ss3.TPCID.Cryostat, ss3.TPCID.TPC, plane);
   int ssid = 0;
   for(auto cid : ss3.CotIDs) {
   auto& ss = slc.cots[cid - 1];
   if(ss.CTP != inCTP) continue;
   ssid = ss.ID;
   break;
   } // cid
   if(ssid == 0) continue;
   auto tpFrom = MakeBareTP(slc, pfp.XYZ[pEnd], pToS, inCTP);
   auto& ss = slc.cots[ssid - 1];
   auto& stp1 = slc.tjs[ss.ShowerTjID - 1].Pts[1];
   float sep = PosSep(tpFrom.Pos, stp1.Pos);
   float toPos = tpFrom.Pos[0] + 0.5 * tpFrom.Dir[0] * sep;
   float cf = ChgFracBetween(slc, tpFrom, toPos, false);
   // weight by the separation in the plane
   totSep += sep;
   chgFrac += sep * cf;
   } // plane
   if(totSep > 0) hist.fChgFrac = chgFrac / totSep;
   hist.fAlong = alongTrans[0];
   hist.fTrans = alongTrans[1];
   hist.fInShwrProb = InShowerProbLong(ss3Energy, -hist.fSep);
   bool isBad = (hist.fDang1 > 2 || hist.fChgFrac < 0.5 || hist.fInShwrProb < 0.05);
   if(pfp.ID == truPFP && isBad) {
   mf::LogVerbatim myprt("TC");
   myprt<<"SSP: 3S"<<ss3.ID<<" shEnergy "<<(int)ss3Energy<<" P"<<pfp.ID<<" pfpEnergy "<<(int)pfpEnergy;
   myprt<<" MCSMom "<<hist.fMCSMom<<" len "<<hist.fPfpLen;
   myprt<<" Dang1 "<<hist.fDang1<<" Dang2 "<<hist.fDang2<<" chgFrac "<<hist.fChgFrac;
   myprt<<" fInShwrProb "<<hist.fInShwrProb;
   myprt<<" EventsProcessed "<<evt.eventsProcessed;
   }
   if(pfp.ID == truPFP) {
   hist.fShowerParentSig->Fill();
   } else {
   hist.fShowerParentBkg->Fill();
   }
   } // pfp
   } // part
   //    PrintShowers(fcnLabel, tjs);
   //    Print2DShowers(fcnLabel, slc, USHRT_MAX, false);
   // kill the cheat showers
   for(auto& ss3 : slc.showers) {
   if(ss3.ID == 0) continue;
   if(!ss3.Cheat) continue;
   for(auto cid : ss3.CotIDs) {
   auto& ss = slc.cots[cid - 1];
   ss.ID = 0;
   auto& stj = slc.tjs[ss.ShowerTjID - 1];
   stj.AlgMod[kKilled] = true;
   } // cid
   ss3.ID = 0;
   } // ss3
   } // StudyShowerParents
   */
} // namespace tca
