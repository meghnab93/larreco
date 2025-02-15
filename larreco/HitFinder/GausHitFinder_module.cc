////////////////////////////////////////////////////////////////////////
//
// GaussHitFinder class
//
// jaasaadi@syr.edu
//
//  This algorithm is designed to find hits on wires after deconvolution.
// -----------------------------------
// This algorithm is based on the FFTHitFinder written by Brian Page,
// Michigan State University, for the ArgoNeuT experiment.
//
//
// The algorithm walks along the wire and looks for pulses above threshold
// The algorithm then attempts to fit n-gaussians to these pulses where n
// is set by the number of peaks found in the pulse
// If the Chi2/NDF returned is "bad" it attempts to fit n+1 gaussians to
// the pulse. If this is a better fit it then uses the parameters of the
// Gaussian fit to characterize the "hit" object
//
// To use this simply include the following in your producers:
// gaushit:     @local::microboone_gaushitfinder
// gaushit:	@local::argoneut_gaushitfinder
////////////////////////////////////////////////////////////////////////

// C/C++ standard library
#include <algorithm> // std::accumulate()
#include <atomic>
#include <memory> // std::unique_ptr()
#include <string>
#include <utility> // std::move()

// Framework includes
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Core/SharedProducer.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art/Utilities/Globals.h"
#include "art/Utilities/make_tool.h"
#include "art_root_io/TFileService.h"
#include "canvas/Persistency/Common/FindOneP.h"
#include "fhiclcpp/ParameterSet.h"

// LArSoft Includes
#include "larcore/Geometry/Geometry.h"
#include "larcoreobj/SimpleTypesAndConstants/RawTypes.h" // raw::ChannelID_t
#include "lardata/ArtDataHelper/HitCreator.h"
#include "lardataobj/RecoBase/Hit.h"
#include "lardataobj/RecoBase/Wire.h"
#include "larreco/HitFinder/HitFilterAlg.h"

#include "larreco/HitFinder/HitFinderTools/ICandidateHitFinder.h"
#include "larreco/HitFinder/HitFinderTools/IPeakFitter.h"

// ROOT Includes
#include "TH1F.h"
#include "TMath.h"

#include "tbb/concurrent_vector.h"
#include "tbb/parallel_for.h"

namespace hit {
  class GausHitFinder : public art::SharedProducer {
  public:
    explicit GausHitFinder(fhicl::ParameterSet const& pset, art::ProcessingFrame const&);

  private:
    struct hitstruct {
      recob::Hit hit_tbb;
      art::Ptr<recob::Wire> wire_tbb;
      bool keep;

      // Constructor that matches the arguments you're trying to pass

      hitstruct(recob::Hit h, art::Ptr<recob::Wire> w, bool k)
	: hit_tbb(std::move(h)), wire_tbb(w), keep(k) {}
    };

    void produce(art::Event& evt, art::ProcessingFrame const&) override;
    tbb::concurrent_vector<hitstruct> processWire(size_t wireIter, const art::Handle<std::vector<recob::Wire>>& wireVecHandle, geo::Geometry const &geom, unsigned int count);

    std::vector<double> FillOutHitParameterVector(const std::vector<double>& input);
    std::function<double(double, double, double, double, int, int)> getCharge;
    const bool fFilterHits;
    //const bool fFillHists;

    const std::string fCalDataModuleLabel;
    const std::string fAllHitsInstanceName;

    const std::vector<int> fLongMaxHitsVec;    ///<Maximum number hits on a really long pulse train
    const std::vector<int> fLongPulseWidthVec; ///<Sets width of hits used to describe long pulses

    const size_t fMaxMultiHit; ///<maximum hits for multi fit
    const int fAreaMethod;     ///<Type of area calculation
    const std::vector<double>
      fAreaNormsVec;       ///<factors for converting area to same units as peak height
    const double fChi2NDF; ///maximum Chisquared / NDF allowed for a hit to be saved

    const std::vector<float> fPulseHeightCuts;
    const std::vector<float> fPulseWidthCuts;
    const std::vector<float> fPulseRatioCuts;

    std::atomic<size_t> fEventCount{0};

    //only Standard and Morphological implementation is threadsafe.
    std::vector<std::unique_ptr<reco_tool::ICandidateHitFinder>>
      fHitFinderToolVec; ///< For finding candidate hits
    // only Marqdt implementation is threadsafe.
    std::unique_ptr<reco_tool::IPeakFitter> fPeakFitterTool; ///< Perform fit to candidate peaks
    //HitFilterAlg implementation is threadsafe.
    std::unique_ptr<HitFilterAlg> fHitFilterAlg; ///< algorithm used to filter out noise hits

    //only used when fFillHists is true and in single threaded mode.
    //TH1F* fFirstChi2;
    //TH1F* fChi2;

  }; // class GausHitFinder

  //-------------------------------------------------
  //-------------------------------------------------
  GausHitFinder::GausHitFinder(fhicl::ParameterSet const& pset, art::ProcessingFrame const&)
    : SharedProducer{pset}
    , fFilterHits(pset.get<bool>("FilterHits", false))
    //, fFillHists(pset.get<bool>("FillHists", false))
    , fCalDataModuleLabel(pset.get<std::string>("CalDataModuleLabel"))
    , fAllHitsInstanceName(pset.get<std::string>("AllHitsInstanceName", ""))
    , fLongMaxHitsVec(pset.get<std::vector<int>>("LongMaxHits", std::vector<int>() = {25, 25, 25}))
    , fLongPulseWidthVec(
	pset.get<std::vector<int>>("LongPulseWidth", std::vector<int>() = {16, 16, 16}))
    , fMaxMultiHit(pset.get<int>("MaxMultiHit"))
    , fAreaMethod(pset.get<int>("AreaMethod"))
    , fAreaNormsVec(FillOutHitParameterVector(pset.get<std::vector<double>>("AreaNorms")))
    , fChi2NDF(pset.get<double>("Chi2NDF"))
    , fPulseHeightCuts(
	pset.get<std::vector<float>>("PulseHeightCuts", std::vector<float>() = {3.0, 3.0, 3.0}))
    , fPulseWidthCuts(
	pset.get<std::vector<float>>("PulseWidthCuts", std::vector<float>() = {2.0, 1.5, 1.0}))
    , fPulseRatioCuts(
	pset.get<std::vector<float>>("PulseRatioCuts", std::vector<float>() = {0.35, 0.40, 0.20}))
  {
    if (art::Globals::instance()->nthreads() > 1u) {
      throw art::Exception(art::errors::Configuration)
	<< "Cannot fill histograms when multiple threads configured, please set fFillHists to "
	   "false or change number of threads to 1\n";
    }
    async<art::InEvent>();
    if (fFilterHits) {
      fHitFilterAlg = std::make_unique<HitFilterAlg>(pset.get<fhicl::ParameterSet>("HitFilterAlg"));
    }

    // recover the tool to do the candidate hit finding
    // Recover the vector of fhicl parameters for the ROI tools
    const fhicl::ParameterSet& hitFinderTools = pset.get<fhicl::ParameterSet>("HitFinderToolVec");

    fHitFinderToolVec.resize(hitFinderTools.get_pset_names().size());

    for (const std::string& hitFinderTool : hitFinderTools.get_pset_names()) {
      const fhicl::ParameterSet& hitFinderToolParamSet =
	hitFinderTools.get<fhicl::ParameterSet>(hitFinderTool);
      size_t planeIdx = hitFinderToolParamSet.get<size_t>("Plane");

      fHitFinderToolVec.at(planeIdx) =
	art::make_tool<reco_tool::ICandidateHitFinder>(hitFinderToolParamSet);
    }

    // Recover the peak fitting tool
    fPeakFitterTool =
      art::make_tool<reco_tool::IPeakFitter>(pset.get<fhicl::ParameterSet>("PeakFitter"));
    if (fAreaMethod == 0) {

      getCharge = [](double peakMean, double peakAmp, double peakWidth, double areaNorm, int low, int hi) {
	double charge = 0;
	for (int sigPos = low; sigPos < hi; sigPos++)
	  charge += peakAmp * TMath::Gaus(sigPos, peakMean, peakWidth);
	return charge;
      };
    }
    else {

      getCharge = [](double peakMean, double peakAmp, double peakWidth, double areaNorm, int low, int hi) {
	return std::sqrt(2 * TMath::Pi()) * peakAmp * peakWidth / areaNorm;
      };
    }
    // let HitCollectionCreator declare that we are going to produce
    // hits and associations with wires and raw digits
    // We want the option to output two hit collections, one filtered
    // and one with all hits. The key to doing this will be a non-null
    // instance name for the second collection
    // (with no particular product label)
    recob::HitCollectionCreator::declare_products(
      producesCollector(), fAllHitsInstanceName, true, false); //fMakeRawDigitAssns);

    // and now the filtered hits...
    if (fAllHitsInstanceName != "")
      recob::HitCollectionCreator::declare_products(
	producesCollector(), "", true, false); //fMakeRawDigitAssns);

    return;
  } // GausHitFinder::GausHitFinder()

  //-------------------------------------------------
  //-------------------------------------------------
  std::vector<double> GausHitFinder::FillOutHitParameterVector(const std::vector<double>& input)
  {
    if (input.size() == 0)
      throw std::runtime_error(
	"GausHitFinder::FillOutHitParameterVector ERROR! Input config vector has zero size.");

    std::vector<double> output;
    art::ServiceHandle<geo::Geometry const> geom;
    const unsigned int N_PLANES = geom->Nplanes();

    if (input.size() == 1)
      output.resize(N_PLANES, input[0]);
    else if (input.size() == N_PLANES)
      output = input;
    else
      throw std::runtime_error("GausHitFinder::FillOutHitParameterVector ERROR! Input config "
			       "vector size !=1 and !=N_PLANES.");
    return output;
  }

  //-------------------------------------------------
  //-------------------------------------------------



  tbb::concurrent_vector<GausHitFinder::hitstruct> GausHitFinder::processWire(size_t wireIter, const art::Handle<std::vector<recob::Wire>>& wireVecHandle, geo::Geometry const& geom, unsigned int count) {
    // ####################################
    // ### Getting this particular wire ###
    // ####################################
    art::Ptr<recob::Wire> wire(wireVecHandle, wireIter);
    // --- Setting Channel Number and Signal type ---
    raw::ChannelID_t channel = wire->Channel();
    // get the WireID for this hit
    std::vector<geo::WireID> wids = geom.ChannelToWire(channel);
    // for now, just take the first option returned from ChannelToWire
    geo::WireID wid = wids[0];
    // We need to know the plane to look up parameters
    geo::PlaneID::PlaneID_t plane = wid.Plane;
    // ---------------------------------------
    // -- Setting the appropriate signal widths and thresholds --
    // --    for the right plane.      --
    // ----------------------------------------------------------
    // #################################################
    // ### Set up to loop over ROI's for this wire   ###
    // #################################################
    const recob::Wire::RegionsOfInterest_t& signalROI = wire->SignalROI();

    tbb::concurrent_vector<hitstruct> processHits_vec;

    tbb::parallel_for(
		      static_cast<std::size_t>(0),
		      signalROI.n_ranges(),
		      [&](size_t& rangeIter) {
			const auto& range = signalROI.range(rangeIter);
			// ROI start time
			raw::TDCtick_t roiFirstBinTick = range.begin_index();
			  // ###########################################################
			// ### Scan the waveform and find candidate peaks + merge  ###
			// ###########################################################

			reco_tool::ICandidateHitFinder::HitCandidateVec hitCandidateVec;
			reco_tool::ICandidateHitFinder::MergeHitCandidateVec mergedCandidateHitVec;

			fHitFinderToolVec.at(plane)->findHitCandidates(
								       range, 0, channel, count, hitCandidateVec);
			fHitFinderToolVec.at(plane)->MergeHitCandidates(
									range, hitCandidateVec, mergedCandidateHitVec);

			// #######################################################
			// ### Lets loop over the pulses we found on this wire ###
			// #######################################################

			for (auto& mergedCands : mergedCandidateHitVec) {
			  int startT = mergedCands.front().startTick;
			  int endT = mergedCands.back().stopTick;

			  // ### Putting in a protection in case things went wrong ###
			  // ### In the end, this primarily catches the case where ###
			  // ### a fake pulse is at the start of the ROI           ###
			  if (endT - startT < 5) continue;

			  // #######################################################
			  // ### Clearing the parameter vector for the new pulse ###
			  // #######################################################

			  // === Setting the number of Gaussians to try ===
			  int nGausForFit = mergedCands.size();

			  // ##################################################
			  // ### Calling the function for fitting Gaussians ###
			  // ##################################################
			  double chi2PerNDF(0.);
			  int NDF(1);
			  /*stand alone
			    reco_tool::IPeakFitter::PeakParamsVec peakParamsVec(nGausForFit);
			  */
			  reco_tool::IPeakFitter::PeakParamsVec peakParamsVec;

			  // #######################################################
			  // ### If # requested Gaussians is too large then punt ###
			  // #######################################################
			  if (mergedCands.size() <= fMaxMultiHit) {
			    fPeakFitterTool->findPeakParameters(
								range.data(), mergedCands, peakParamsVec, chi2PerNDF, NDF);

			    // If the chi2 is infinite then there is a real problem so we bail
			    if (!(chi2PerNDF < std::numeric_limits<double>::infinity())) {
			      chi2PerNDF = 2. * fChi2NDF;
			      NDF = 2;
			    }
			  
			  }



			  // #######################################################
			  // ### If too large then force alternate solution      ###
			  // ### - Make n hits from pulse train where n will     ###
			  // ###   depend on the fhicl parameter fLongPulseWidth ###
			  // ### Also do this if chi^2 is too large              ###
			  // #######################################################
			  if (mergedCands.size() > fMaxMultiHit || nGausForFit * chi2PerNDF > fChi2NDF) {
			  
			    int longPulseWidth = fLongPulseWidthVec.at(plane);
			    int nHitsThisPulse = (endT - startT) / longPulseWidth;

			    if (nHitsThisPulse > fLongMaxHitsVec.at(plane)) {
			      nHitsThisPulse = fLongMaxHitsVec.at(plane);
			      longPulseWidth = (endT - startT) / nHitsThisPulse;
			    }
			    nHitsThisPulse += ((endT - startT) % fLongPulseWidthVec.at(plane)) > 0 ? 1 : 0; // Adjust for partial hit

			    int firstTick = startT;
			    int lastTick = std::min(firstTick + longPulseWidth, endT);

			    peakParamsVec.clear();
			    nGausForFit = nHitsThisPulse;
			    NDF = 1.;
			    chi2PerNDF = chi2PerNDF > fChi2NDF ? chi2PerNDF : -1.;

			    for (int hitIdx = 0; hitIdx < nHitsThisPulse; hitIdx++) {
			      // This hit parameters
			      double sumADC =
				std::accumulate(range.begin() + firstTick, range.begin() + lastTick, 0.);
			      double peakSigma = (lastTick - firstTick) / 3.; // Set the width...
			      double peakAmp = 0.3989 * sumADC / peakSigma;   // Use gaussian formulation
			      double peakMean = (firstTick + lastTick) / 2.;

			      // Store hit params
			      reco_tool::IPeakFitter::PeakFitParams_t peakParams;

			      peakParams.peakCenter = peakMean;
			      peakParams.peakCenterError = 0.1 * peakMean;
			      peakParams.peakSigma = peakSigma;
			      peakParams.peakSigmaError = 0.1 * peakSigma;
			      peakParams.peakAmplitude = peakAmp;
			      peakParams.peakAmplitudeError = 0.1 * peakAmp;

			      peakParamsVec.push_back(peakParams);

			      // set for next loop
			      firstTick = lastTick;
			      lastTick = std::min(lastTick + longPulseWidth, endT);
			    }
			  }

			  // #######################################################
			  // ### Loop through returned peaks and make recob hits ###
			  // #######################################################

			  int numHits(0);

			  // Make a container for what will be ALL the hit collection
			  std::vector<hitstruct> Hits;
			  for (const auto& peakParams : peakParamsVec) {
			    // Extract values for this hit
			    float peakAmp = peakParams.peakAmplitude;
			    float peakMean = peakParams.peakCenter;
			    float peakWidth = peakParams.peakSigma;

			    // Place one bit of protection here
			    if (std::isnan(peakAmp)) {
			      std::cout << "**** hit peak amplitude is a nan! Channel: " << channel
					<< ", start tick: " << startT << std::endl;
			      continue;
			    }

			    // Extract errors
			    float peakAmpErr = peakParams.peakAmplitudeError;
			    float peakMeanErr = peakParams.peakCenterError;
			    float peakWidthErr = peakParams.peakSigmaError;

			    // ### Charge ###
			    float charge =
			      getCharge(peakMean, peakAmp, peakWidth, fAreaNormsVec[plane], startT, endT);

			    ;
			    float chargeErr =
			      std::sqrt(TMath::Pi()) * (peakAmpErr * peakWidthErr + peakWidthErr * peakAmpErr);

			    // ### limits for getting sums
			    std::vector<float>::const_iterator sumStartItr = range.begin() + startT;
			    std::vector<float>::const_iterator sumEndItr = range.begin() + endT;

			    // ### Sum of ADC counts
			    double sumADC = std::accumulate(sumStartItr, sumEndItr, 0.);

			    // ok, now create the hit
			    recob::HitCreator hitcreator(
							 *wire,                      // wire reference
							 wid,                        // wire ID
							 startT + roiFirstBinTick,   // start_tick TODO check
							 endT + roiFirstBinTick,     // end_tick TODO check
							 peakWidth,                  // rms
							 peakMean + roiFirstBinTick, // peak_time
							 peakMeanErr,                // sigma_peak_time
							 peakAmp,                    // peak_amplitude
							 peakAmpErr,                 // sigma_peak_amplitude
							 charge,                     // hit_integral
							 chargeErr,                  // hit_sigma_integral
							 sumADC,                     // summedADC FIXME
							 nGausForFit,                // multiplicity
							 numHits,                    // local_index TODO check that the order is correct
							 chi2PerNDF,                 // goodness_of_fit
							 NDF                         // dof
							 );

			    Hits.emplace_back(hitcreator.move(), wire, false); //a local vector of hitsruct

			    numHits++;

			  } // <---End loop over gaussians

			  // Should we filter hits?
			  if (!fHitFilterAlg || Hits.empty()) {
			    processHits_vec.grow_by(Hits.begin(), Hits.end());
			    continue;
			  }

			  


			  // #######################################################################
			  // Is all this sorting really necessary?  Would it be faster to just loop
			  // through the hits and perform simple cuts on amplitude and width on a
			  // hit-by-hit basis, either here in the module (using fPulseHeightCuts and
			  // fPulseWidthCuts) or in HitFilterAlg?
			  // #######################################################################


			  // Sort in ascending peak height
			  std::sort(Hits.begin(),
				    Hits.end(),
				    [](const auto& left, const auto& right) {
				      return left.hit_tbb.PeakAmplitude() > right.hit_tbb.PeakAmplitude();
				    });

			  // #####################################################
			  // This is redundant in the new logic - we can get rid of this part without any harm
			  // we can't continue here as we want to still store the hits that fail this statement
			  // ######################################################

			  // Reject if the first hit fails the PH/wid cuts

			  if (Hits.front().hit_tbb.PeakAmplitude() < fPulseHeightCuts.at(plane) ||
			      Hits.front().hit_tbb.RMS() < fPulseWidthCuts.at(plane)) {
			    processHits_vec.grow_by(Hits.begin(), Hits.end());

			    continue; 
			  }
			  // Now check other hits in the snippet

			  // The largest pulse height will now be at the front...
			  float largestPH = Hits.front().hit_tbb.PeakAmplitude();

			  // Find where the pulse heights drop below threshold
			  float threshold(fPulseRatioCuts.at(plane));

			  for (auto& hit : Hits) {
			    hit.keep = !(hit.hit_tbb.PeakAmplitude() < 8. && hit.hit_tbb.PeakAmplitude() / largestPH < threshold) && fHitFilterAlg->IsGoodHit(hit.hit_tbb);
			    

			  }

			  processHits_vec.grow_by(Hits.begin(), Hits.end());

			} //<---End loop over merged candidate hits
		      }   //<---End looping over ROI's // lambda function
		      ); // end loop tbb parallel for 
    return processHits_vec;

  }

  //  This algorithm uses the fact that deconvolved signals are very smooth
  //  and looks for hits as areas between local minima that have signal above
  //  threshold.
  //-------------------------------------------------
  void GausHitFinder::produce(art::Event& evt, art::ProcessingFrame const&)
  {
    unsigned int count = fEventCount.fetch_add(1);
    //==================================================================================================

    TH1::AddDirectory(kFALSE);

    // Instantiate and Reset a stop watch
    //TStopwatch StopWatch;
    //StopWatch.Reset();

    // ################################
    // ### Calling Geometry service ###
    // ################################
    art::ServiceHandle<geo::Geometry const> geom;

    // ###############################################
    // ### Making a ptr vector to put on the event ###
    // ###############################################

    //store in a thread safe way                                                                                                                                                                            


    tbb::concurrent_vector<hitstruct> hitstruct_vec;
    //tbb::concurrent_vector<hitstruct> filthitstruct_vec;

    //    if (fAllHitsInstanceName != "") filteredHitCol = &hcol;

    // ##########################################
    // ### Reading in the Wire List object(s) ###
    // ##########################################
    art::Handle<std::vector<recob::Wire>> wireVecHandle;
    evt.getByLabel(fCalDataModuleLabel, wireVecHandle);

    //#################################################
    //###    Set the charge determination method    ###
    //### Default is to compute the normalized area ###
    //#################################################

    //##############################
    //### Looping over the wires ###
    //##############################
    //for(size_t wireIter = 0; wireIter < wireVecHandle->size(); wireIter++)
    //{
    tbb::parallel_for(
		      static_cast<size_t>(0), wireVecHandle->size(), [&](size_t wireIter) {
			auto hits = processWire(wireIter, wireVecHandle, *geom, count);
			hitstruct_vec.grow_by(hits.begin(), hits.end());
		      } // close lambda function (wire)
		      );// end loop tbb parallel for
    
    // this contains the hit collection
    // and its associations to wires and raw digits
    if (fFilterHits) {
      recob::HitCollectionCreator filteredHitCol(evt, "", true, false);

      // Iterate over hitstruct_vec and only emplace_back if keep == true

      for (size_t j = 0; j < hitstruct_vec.size(); j++) {
	if (hitstruct_vec[j].keep) { // Check if the hit should be kept
	  filteredHitCol.emplace_back(hitstruct_vec[j].hit_tbb, hitstruct_vec[j].wire_tbb);
	}
      }

      filteredHitCol.put_into(evt);

      if (fAllHitsInstanceName.empty()) {
	return;
      }
    }

    recob::HitCollectionCreator allHitCol(evt, fAllHitsInstanceName, true, false);
    for (size_t i = 0; i < hitstruct_vec.size(); i++) {
      allHitCol.emplace_back(hitstruct_vec[i].hit_tbb, hitstruct_vec[i].wire_tbb);
    }
    allHitCol.put_into(evt);


  } // End of produce()

  DEFINE_ART_MODULE(GausHitFinder)

} // end of hit namespace
B
