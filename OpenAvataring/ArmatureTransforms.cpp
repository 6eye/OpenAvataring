#include "pch.h"

#include "ArmatureTransforms.h"
#include "CharacterController.h"
#include "EigenExtension.h"

#include <set>
#include <random>
#include <iostream>

#include <PrimitiveVisualizer.h>
#include <Causality\CharacterObject.h>
#include <Causality\Settings.h>
#include <Causality\MatrixVisualizer.h>
#include <Causality\Scene.h>

using namespace std;
using namespace Causality;
using namespace ArmaturePartFeatures;
using namespace Eigen;

namespace Eigen
{
	template <typename ValueType, int Rows, int Cols>
	inline gsl::span<const ValueType, Cols, Rows> as_span(const Eigen::Matrix<ValueType, Rows, Cols, Eigen::ColMajor>& mat)
	{
		typedef gsl::span<const ValueType, Cols, Rows> span_type;
		typedef typename span_type::bounds_type bounds_type;
		return span_type(mat.data(), bounds_type({ (std::ptrdiff_t)mat.cols(), (std::ptrdiff_t)mat.rows() }));
	};

	//template <typename ValueType, int Rows, int Cols>
	//inline gsl::span<const ValueType, Rows, Cols> as_span(const Eigen::Matrix<ValueType, Rows, Cols, Eigen::RowMajor>& mat)
	//{
	//	typedef gsl::span<const ValueType, Cols, Rows> span_type;
	//	typedef typename span_type::bounds_type bounds_type;
	//	return gsl::span<ValueType, Rows, Cols>(mat.data(), bounds_type({ (std::ptrdiff_t)mat.cols(), (std::ptrdiff_t)mat.rows() }));
	//};

	//template <typename ValueType, int Rows, int Cols>
	//inline gsl::span<const ValueType, Cols, Rows> as_span(const Eigen::Array<ValueType, Rows, Cols, Eigen::ColMajor>& mat)
	//{
	//	typedef gsl::span<const ValueType, Cols, Rows> span_type;
	//	typedef typename span_type::bounds_type bounds_type;
	//	return gsl::span<ValueType, Cols, Rows>(mat.data(), bounds_type({ (std::ptrdiff_t)mat.rows(), (std::ptrdiff_t)mat.cols() }));
	//};

	//template <typename ValueType, int Rows, int Cols>
	//inline gsl::span<const ValueType, Rows, Cols> as_span(const Eigen::Array<ValueType, Rows, Cols, Eigen::RowMajor>& mat)
	//{
	//	typedef gsl::span<const ValueType, Cols, Rows> span_type;
	//	typedef typename span_type::bounds_type bounds_type;
	//	return gsl::span<ValueType, Rows, Cols>(mat.data(), bounds_type({ (std::ptrdiff_t)mat.rows(), (std::ptrdiff_t)mat.cols() }));
	//};
}

using Eigen::as_span;

static constexpr size_t MAX_FRAME_RATE = 30;

bool g_GestureMappingOnly = true;
int  g_TrackerTopK = 30;

double g_DynamicTraderKeyEnergy = 0.2;
extern double g_CurvePower;

extern RowVector3d	g_NoiseInterpolation;
namespace Causality
{
	extern float g_DebugArmatureThinkness;
}

MatrixXf g_Sample;

typedef
	Localize<
	EndEffector<InputFeature>>
InputExtractorType;

typedef
Pcad <
	Weighted <
	RelativeDeformation <
	AllJoints < CharacterFeature > > > >
	OutputExtractorType;


#define BEGIN_TO_END(range) range.begin(), range.end()

#ifdef _DEBUG
#define DEBUGOUT(x) std::cout << #x << " = " << x << std::endl
#else
#define DEBUGOUT(x)
#endif

template <class Derived>
void ExpandQuadraticTerm(_Inout_ DenseBase<Derived> &v, _In_ DenseIndex dim)
{
	assert(v.cols() == dim + (dim * (dim + 1) / 2));

	int k = dim;
	for (int i = 0; i < dim; i++)
	{
		for (int j = i; j < dim; j++)
		{
			v.col(k) = v.col(i).array() * v.col(j).array();
			++k;
		}
	}
}

template <typename T>
T sqr(T x)
{
	return x * x;
}

template <typename Derived>
int GetFrameVector(IArmaturePartFeature* feature, _Out_ Eigen::DenseBase<Derived>& v, ArmatureFrameConstView frame, const ShrinkedArmature& parts)
{
	VectorXf vp;
	int idx = 0;
	for (auto& part : parts)
	{
		vp = feature->Get(*part, frame);
		v.segment(idx, vp.size()) = vp;
		idx += vp.size();
	}
	return idx;
}

template <typename Derived>
int SetFrameVector(IArmaturePartFeature* feature, _In_ const Eigen::DenseBase<Derived>& v, ArmatureFrameView frame, const ShrinkedArmature& parts)
{
	RowVectorXf vp;
	int idx = 0;
	for (auto& part : parts)
	{
		int sz = feature->GetDimension(*part);
		vp = v.segment(idx, sz);
		idx += sz;
		feature->Set(*part, frame, vp);
	}
	return idx;
}


EndEffectorGblPosQuadratized::EndEffectorGblPosQuadratized()
{
}

int EndEffectorGblPosQuadratized::GetDimension(const ArmaturePart& block) const
{
	return 9;
}

RowVectorXf EndEffectorGblPosQuadratized::Get(const ArmaturePart & block, ArmatureFrameConstView frame)
{
	using namespace Eigen;
	RowVectorXf Y(BoneFeatureType::Dimension);

	BoneFeatures::GblPosFeature::Get(Y.segment<3>(0), frame[block.Joints.back()->ID]);

	if (block.parent() != nullptr)
	{
		RowVectorXf reference(BoneFeatures::GblPosFeature::Dimension);
		BoneFeatures::GblPosFeature::Get(reference, frame[block.parent()->Joints.back()->ID]);
		Y.segment<3>(0) -= reference;
	}

	ExpandQuadraticTerm(Y, 3);

	//Y[3] = Y[0] * Y[0];
	//Y[4] = Y[1] * Y[1];
	//Y[5] = Y[2] * Y[2];
	//Y[6] = Y[0] * Y[1];
	//Y[7] = Y[1] * Y[2];
	//Y[8] = Y[2] * Y[0];
	return Y;
}

// Inherited via IArmaturePartFeature

void EndEffectorGblPosQuadratized::Set(const ArmaturePart & block, ArmatureFrameView frame, const RowVectorXf & feature)
{
	assert(false);
}

BlockizedArmatureTransform::BlockizedArmatureTransform() :m_sParts(nullptr), m_cParts(nullptr)
{
	m_sArmature = nullptr;
	m_tArmature = nullptr;
}

BlockizedArmatureTransform::BlockizedArmatureTransform(const ShrinkedArmature * pSourceBlock, const ShrinkedArmature * pTargetBlock)
{
	SetFrom(pSourceBlock, pTargetBlock);
}

void BlockizedArmatureTransform::SetFrom(const ShrinkedArmature * pSourceBlock, const ShrinkedArmature * pTargetBlock)
{
	m_sParts = pSourceBlock; m_cParts = pTargetBlock;
	m_sArmature = &pSourceBlock->Armature();
	m_tArmature = &pTargetBlock->Armature();
}

void BlockizedCcaArmatureTransform::Transform(frame_view target_frame, const_frame_view source_frame)
{
	using namespace Eigen;
	using namespace std;
	const auto& sblocks = *m_sParts;
	const auto& tblocks = *m_cParts;
	RowVectorXf X, Y;
	for (const auto& map : Maps)
	{
		if (map.Jx == -1 || map.Jy == -1) continue;

		auto pTb = tblocks[map.Jy];
		Y = pOutputExtractor->Get(*pTb, target_frame);

		if (map.Jx == -2 && map.Jy >= 0)
		{
			vector<RowVectorXf> Xs;
			int Xsize = 0;
			for (auto& pBlock : tblocks)
			{
				if (pBlock->ActiveActions.size() > 0)
				{
					Xs.emplace_back(pInputExtractor->Get(*pBlock, source_frame));
					Xsize += Xs.back().size();
				}
			}

			X.resize(Xsize);
			Xsize = 0;
			for (auto& xr : Xs)
			{
				X.segment(Xsize, xr.size()) = xr;
				Xsize += xr.size();
			}
		}
		else
		{
			auto pSb = sblocks[map.Jx];
			X = pInputExtractor->Get(*pSb, source_frame);
		}

		map.Apply(X, Y);

		//cout << " X = " << X << endl;
		//cout << " Yr = " << Y << endl;
		pOutputExtractor->Set(*pTb, target_frame, Y);

	}

	//target_frame[0].LclTranslation = source_frame[0].GblTranslation;
	//target_frame[0].LclRotation = source_frame[0].LclRotation;
	FrameRebuildGlobal(*m_tArmature, target_frame);
}

void BlockizedCcaArmatureTransform::Transform(frame_view target_frame, const_frame_view source_frame, const_frame_view last_frame, float frame_time)
{
	// redirect to pose transform
	Transform(target_frame, source_frame);
	return;

	const auto& sblocks = *m_sParts;
	const auto& tblocks = *m_cParts;
	for (const auto& map : Maps)
	{
		if (map.Jx == -1 || map.Jy == -1) continue;

		auto pSb = sblocks[map.Jx];
		auto pTb = tblocks[map.Jy];
		auto X = pInputExtractor->Get(*pSb, source_frame, last_frame, frame_time);
		auto Y = pOutputExtractor->Get(*pTb, target_frame);

		map.Apply(X, Y);

		//cout << " X = " << X << endl;
		//cout << " Yr = " << Y << endl;
		pOutputExtractor->Set(*pTb, target_frame, Y);
	}

	//target_frame[0].LclTranslation = source_frame[0].GblTranslation;
	//target_frame[0].LclRotation = source_frame[0].LclRotation;
	FrameRebuildGlobal(*m_tArmature, target_frame);
}

ArmaturePartFeatures::PerceptiveVector::PerceptiveVector(CharacterController & controller)
	: m_controller(controller)
{

}

inline int ArmaturePartFeatures::PerceptiveVector::GetDimension() const
{
	return Quadratic ? 9 : 3;
}

inline int ArmaturePartFeatures::PerceptiveVector::GetDimension(const ArmaturePart & block) const
{
	return GetDimension();
}

RowVectorXf PerceptiveVector::Get(const ArmaturePart & block, ArmatureFrameConstView frame)
{
	using namespace Eigen;

	DenseIndex dim = InputFeatureType::Dimension;
	if (Quadratic)
		dim += dim * (dim + 1) / 2;

	RowVectorXf Y(dim);

	auto& aJoint = *block.Joints.back();

	BoneFeatures::GblPosFeature::Get(Y.segment<3>(0), frame[aJoint.ID]);

	if (block.parent() != nullptr)
	{
		RowVectorXf reference(BoneFeatures::GblPosFeature::Dimension);
		BoneFeatures::GblPosFeature::Get(reference, frame[block.parent()->Joints.back()->ID]);
		Y.segment<3>(0) -= reference;
	}

	if (Quadratic)
	{
		ExpandQuadraticTerm(Y, InputFeatureType::Dimension);
	}

	return Y;
}

// Inherited via IArmaturePartFeature


template <class CharacterFeatureType>
Vector3f GetChainEndPositionFromY(ArmatureFrameConstView dFrame, const std::vector<Joint*> & joints, const RowVectorXf & Y)
{
	assert(joints.size() * CharacterFeatureType::Dimension == Y.size());

	// a double buffer
	Bone bones[2];
	int sw = 0;

	for (int i = 0; i < joints.size(); i++)
	{
		auto jid = joints[i]->ID;

		// set scale and translation
		bones[sw].LocalTransform() = dFrame[jid].LocalTransform();

		auto Yj = Y.segment<CharacterFeatureType::Dimension>(i * CharacterFeatureType::Dimension);
		// set local rotation
		CharacterFeatureType::Set(bones[sw], Yj);

		// update global data
		bones[sw].UpdateGlobalData(bones[1 - sw]);
	}

	Vector3f pos = Vector3f::MapAligned(&bones[sw].GblTranslation.x);
	return pos;
}


void PerceptiveVector::Set(const ArmaturePart & block, ArmatureFrameView frame, const RowVectorXf& feature)
{
	using namespace DirectX;
	using namespace Eigen;

	DirectX::Quaternion baseRot;
	if (block.Joints.front()->ParentID >= 0)
		frame[block.Joints.front()->ParentID].GblRotation;
	if (block.ActiveActions.size() > 0)
	{
		RowVectorXf Y(CharacterFeatureType::Dimension * block.Joints.size());

		if (block.ActiveActions.size() + block.SubActiveActions.size() == 0) return;

		if (!g_UseStylizedIK)
		{
			//cpart.PdCca.Apply(feature, Y);
		}
		else
		{
			RowVectorXd dY;//(CharacterFeatureType::Dimension * cpart.Joints.size());
						   // since the feature is expanded to quadritic form, we remove the extra term
			double varZ = 10;

			//MatrixXd covObsr(g_PvDimension, g_PvDimension);
			//covObsr = g_NoiseInterpolation.replicate(1, 2).transpose().asDiagonal() * varZ;

			if (g_PvDimension == 6 && g_UseVelocity)
			{
				auto dX = feature.cast<double>().eval();
				auto& sik = m_controller.GetStylizedIK(block.Index);
				auto& gpr = sik.Gpr();
				//sik.setBaseRotation(frame[block.parent()->Joints.back()->ID].GblRotation);
				dY = sik.apply(dX.segment<3>(0).transpose().eval(), dX.segment<3>(3).transpose().eval(), baseRot).cast<double>();
			}
			double likilyhood = 1.0;

			//auto likilyhood = cpart.PdGpr.get_expectation_from_observation(feature.cast<double>(), covObsr, &dY);
			//auto likilyhood = cpart.PdGpr.get_expectation_and_likelihood(feature.cast<double>(), &dY);
			Y = dY.cast<float>();
			//Y *= cpart.Wx.cwiseInverse().asDiagonal(); // Inverse scale feature

			if (g_StepFrame)
				std::cout << block.Joints[0]->Name << " : " << likilyhood << std::endl;
		}

		for (size_t j = 0; j < block.Joints.size(); j++)
		{
			auto jid = block.Joints[j]->ID;
			auto Xj = Y.segment<CharacterFeatureType::Dimension>(j * CharacterFeatureType::Dimension);
			CharacterFeatureType::Set(frame[jid], Xj);
		}
	}
}

PartilizedTransformer::~PartilizedTransformer()
{

}

PartilizedTransformer::PartilizedTransformer(const ShrinkedArmature& sParts, CharacterController & controller)
	: m_controller(controller)
{

	m_handles = controller.PvHandles();
	m_matvis = const_cast<MatrixVisualizer*>(controller.Character().FirstChildOfType<MatrixVisualizer>());

	auto& parts = controller.ArmatureParts();
	auto& armature = controller.Armature();
	m_sArmature = &sParts.Armature();
	m_tArmature = &armature;
	m_cParts = &parts;
	m_sParts = &sParts;

	m_trackerSwitchThreshold = g_TrackerSwitchCondifidentThreshold;
	m_trackerSwitchTimeThreshold = g_TrackerSwitchTimeThreshold;

	m_updateFreq = 30;
	m_updateFreqf = 30.0f;
	m_speedFilter.SetUpdateFrequency(&m_updateFreq);
	m_speedFilter.SetCutoffFrequency(g_DynamicTraderSpeedFilterCutoffFrequency);
	m_speedFilter.Reset();

	m_jointFilters.resize(m_tArmature->size());
	for (int i = 0; i < m_tArmature->size(); i++)
	{
		m_jointFilters[i].SetUpdateFrequency(&m_updateFreqf);
		m_jointFilters[i].SetCutoffFrequency(g_CharacterJointFilterCutoffFrequency);
		m_jointFilters[i].Reset();
	}


	auto pIF = std::make_shared<InputExtractorType>();
	auto pOF = std::make_shared<OutputExtractorType>();

	auto& ucinfo = controller.GetUnitedClipinfo();

	pOF->InitPcas(parts.size());
	pOF->SetDefaultFrame(armature.bind_frame());
	pOF->InitializeWeights(parts);

	auto& facade = ucinfo.RcFacade;
	auto cutoff = facade.PcaCutoff();
	for (auto part : parts)
	{
		int pid = part->Index;
		if (part->ActiveActions.size() > 0 || part->SubActiveActions.size() > 0)
		{
			auto &pca = facade.GetPartPca(pid);
			auto d = facade.GetPartPcaDim(pid);
			pOF->SetPca(pid, pca.components(d), pca.mean());
		}
		else
		{
			int odim = facade.GetPartDimension(pid);
			pOF->SetPca(pid, MatrixXf::Identity(odim, odim), facade.GetPartMean(pid));
		}
	}

	m_pActiveF = pOF;
	m_pInputF = pIF;
}

void PartilizedTransformer::Transform(frame_view target_frame, const_frame_view source_frame)
{
	Transform(target_frame, source_frame, source_frame, .0f);
}

void PartilizedTransformer::Transform(frame_view target_frame, const_frame_view source_frame, const_frame_view last_frame, float frame_time)
{
	int computeVelocity = frame_time != .0f && g_UseVelocity;

	const auto& cparts = *m_cParts;
	const auto& sparts = *m_sParts;

	auto pvDim = m_pInputF->GetDimension(*sparts[0]);

	m_updateFreq = 1 / frame_time;
	m_updateFreqf = m_updateFreq;

	m_ikDrivedFrame = target_frame;
	m_trackerFrame = target_frame;
	VectorXd actPartsEnergy(cparts.size());
	actPartsEnergy.setOnes();
	actPartsEnergy *= g_DynamicTraderKeyEnergy;

	//cout << frame_time << '|';
	for (auto& ctrl : this->ActiveParts)
	{
		assert(ctrl.DstIdx >= 0 && ctrl.SrcIdx >= 0);
		auto& cpart = const_cast<ArmaturePart&>(*cparts[ctrl.DstIdx]);

		auto energy = GetInputKinectEnergy(ctrl, source_frame, last_frame, frame_time);
		energy = m_speedFilter.Apply(energy);
		actPartsEnergy[ctrl.DstIdx] = energy;

		//cout << energy << ' ';

		RowVectorXf xf = GetInputVector(ctrl, source_frame, last_frame, computeVelocity ? frame_time : 0, computeVelocity);
		SetHandleVisualization(cpart, xf);

		//if (!m_useTracker)
		DriveActivePartSIK(cpart, m_ikDrivedFrame, xf, computeVelocity);
	}

	//cout << endl;

	if (g_EnableDependentControl && m_useTracker)
	{
		P2PTransform ctrl;
		ctrl.SrcIdx = PvInputTypeEnum::ActiveParts;
		auto scv = computeVelocity;
		computeVelocity = g_TrackerUseVelocity ? g_NormalizeVelocity ? 2 : 1 : 0; //! HACK to force use velocity
		auto _x = GetInputVector(ctrl, source_frame, last_frame, computeVelocity ? frame_time : 0, computeVelocity).cast<IGestureTracker::ScalarType>().eval();

		DrivePartsTrackers(_x, frame_time, m_trackerFrame);
		SetTrackerVisualization();
		computeVelocity = scv;
	}

	if (g_EnableDependentControl && !m_useTracker && !AccesseryParts.empty())
	{
		auto& ctrl = AccesseryParts[0];
		auto _x = GetInputVector(ctrl, source_frame, last_frame, computeVelocity ? frame_time : 0, computeVelocity);
		_x = (_x - m_controller.uXabpv) * m_controller.XabpvT;
		RowVectorXd Xd = _x.cast<double>();

		for (auto ctrl : AccesseryParts)
		{
			auto& cpart = const_cast<ArmaturePart&>(*cparts[ctrl.DstIdx]);

			DriveAccesseryPart(cpart, Xd, target_frame);
		}
	}

	actPartsEnergy = /*1.0 - */actPartsEnergy.array()/g_DynamicTraderKeyEnergy;
	actPartsEnergy = actPartsEnergy.cwiseMax(.0).cwiseMin(1.0);

	if (m_useTracker && m_pTrackerFeature)
		BlendFrame(*m_pTrackerFeature, *m_cParts, target_frame, array_view<double>(actPartsEnergy.data(),actPartsEnergy.size()), m_ikDrivedFrame, m_trackerFrame);
	else
	{
		target_frame = m_ikDrivedFrame;
	}

	// Apply joint filter
	for (int i = 0; i < m_tArmature->size(); i++)
	{
		target_frame[i].LclRotation = m_jointFilters[i].Apply(target_frame[i].LclRotation);
	}

	FrameRebuildGlobal(*m_tArmature, target_frame);

	//target_frame[0].LclTranslation = source_frame[0].LclTranslation;
	//target_frame[0].GblTranslation = source_frame[0].GblTranslation;
}

void PartilizedTransformer::SetTrackerVisualization()
{
	if (g_DebugView)
	{
		m_matvis->SetEnabled();

		auto& tracker = m_Trackers[m_currentTracker];

		auto& sample = tracker.GetSampleMatrix();
		const auto& liks = tracker.GetSampleLikilihoods();
		auto timeline = sample.array().col(0);
		auto scls = sample.array().col(1);
		auto vts = sample.array().col(2);

		// Likilihoods
		// Velocity
		// Scale
		// Timeline
		if (g_Sample.rows() < sample.rows())
			g_Sample.resize(sample.rows(), sample.cols() + 1);

		auto wa = liks.maxCoeff();
		g_Sample.col(0) = (liks / wa).cast<float>();
		g_Sample.col(1) = (vts.cast<float>()) / ( 2.0f * g_TrackerVtThreshold) + 0.5;
		g_Sample.col(2) = (scls.cast<float>() - 1.0f) / (4.0f * g_TrackerStDevScale) + 0.5;
		g_Sample.col(3) = timeline.cast<float>() / (float)tracker.Animation().Length().count();
		g_Sample = g_Sample.cwiseMax(.0).cwiseMin(1.0);

		auto n = g_Sample.rows();
		g_Sample.transposeInPlace();
		auto *pair = reinterpret_cast<Vector4*>(g_Sample.data());
		std::sort(pair, pair + n, [](const auto& a, const auto& b) {
			return a.w < b.w;
		});
		g_Sample.transposeInPlace();

		m_matvis->SetAutoContrast(false);
		m_matvis->SetScale(Vector3(1920.0f / (float)n, 16, 1));
		m_matvis->UpdateMatrix(as_span(g_Sample));
	}
	else
	{
		m_matvis->SetEnabled(false);
	}

}

void Causality::BlendFrame(IArmaturePartFeature& feature, const ShrinkedArmature& parts, ArmatureFrameView target, array_view<double> t, ArmatureFrameConstView f0, ArmatureFrameConstView f1)
{
	VectorXf fv0,fv1;
	for (int i = 0; i < parts.size(); i++)
	{
		fv0 = feature.Get(*parts[i], f0);
		fv1 = feature.Get(*parts[i], f1);
		fv0 = fv0 * (1 - t[i]) + fv1 * t[i];
		feature.Set(*parts[i], target, fv0);
	}

}

void PartilizedTransformer::DriveAccesseryPart(ArmaturePart & cpart, Eigen::RowVectorXd &Xd, ArmatureFrameView target_frame)
{
	auto& sik = m_controller.GetStylizedIK(cpart.Index);
	auto& gpr = sik.Gpr();

	RowVectorXd Y;
	auto lk = gpr.get_expectation_and_likelihood(Xd, &Y);

	m_pDrivenF->Set(cpart, target_frame, Y.cast<float>());
}

void PartilizedTransformer::DriveActivePartSIK(ArmaturePart & cpart, ArmatureFrameView target_frame, Eigen::RowVectorXf &xf, bool computeVelocity)
{
	RowVectorXd Xd, Y;

	auto& sik = const_cast<StylizedChainIK&>(m_controller.GetStylizedIK(cpart.Index));
	auto& gpr = sik.Gpr();
	auto& joints = cpart.Joints;

	auto baseRot = target_frame[cpart.parent()->Joints.back()->ID].GblRotation;

	for (int i = 0; i < target_frame.size(); i++)
	{
		assert(!DirectX::XMVector4IsNaN(target_frame[i].GblRotation));
		assert(!DirectX::XMVector4IsNaN(target_frame[i].GblTranslation));
	}

	//sik.SetBaseRotation(baseRot);
	sik.setChain(cpart.Joints, target_frame);

	Xd = xf.cast<double>();
	if (!computeVelocity)
		Y = sik.apply(Xd.transpose(), baseRot).cast<double>();
	else
	{
		assert(xf.size() % 2 == 0);
		auto pvDim = xf.size() / 2;
		Y = sik.apply(Xd.segment(0, pvDim).transpose(), Vector3d(Xd.segment(pvDim, pvDim).transpose()), baseRot).cast<double>();
	}

	assert(!Y.hasNaN());

	m_pActiveF->Set(cpart, target_frame, Y.cast<float>());

	for (int i = 0; i < joints.size(); i++)
		target_frame[joints[i]->ID].UpdateGlobalData(target_frame[joints[i]->parent()->ID]);

}

void PartilizedTransformer::SetHandleVisualization(ArmaturePart & cpart, Eigen::RowVectorXf &xf)
{
	if (!m_handles.empty())
	{
		//auto& handle = m_handles[cpart.Index];
		std::pair<Vector3, Vector3> handle;

		handle.first = Vector3(xf.data());
		if (g_UseVelocity && g_PvDimension == 6)
		{
			handle.second = Vector3(xf.data() + 3);
		}
		else
		{
			handle.second = Vector3::Zero;
		}
		
		m_controller.push_handle(cpart.Index, handle);
	}
}

void PartilizedTransformer::TransformCtrlHandel(RowVectorXf &xf, const Eigen::MatrixXf& homo)
{
	xf *= homo.topLeftCorner(homo.rows() - 1, homo.cols() - 1);
	xf += homo.block(homo.rows() - 1, 0, 1, homo.cols() - 1);
}

using namespace std;

void PartilizedTransformer::GenerateDrivenAccesseryControl()
{
	auto& allclip = m_controller.GetUnitedClipinfo();
	auto& controller = m_controller;

	int pvDim = allclip.PvFacade.GetAllPartDimension();

	if (this->ActiveParts.size() == 0)
		return;
	// Build aparts, dparts
	VectorXi aparts(this->ActiveParts.size());
	set<int> drivSet(BEGIN_TO_END(allclip.ActiveParts()));
	for (int i = 0; i < this->ActiveParts.size(); i++)
	{
		aparts[i] = this->ActiveParts[i].DstIdx;
		drivSet.erase(aparts[i]);
	}
	VectorXi dparts(drivSet.size());
	int i = 0;
	for (auto& pid : drivSet)
		dparts[i++] = pid;

	// Select Xapvs & dpavs
	MatrixXf Xapvs(allclip.ClipFrames(), pvDim * aparts.size());
	MatrixXi cache = aparts.replicate(1, 3).transpose();
	selectCols(allclip.PvFacade.GetAllPartsSequence(), VectorXi::Map(cache.data(), cache.size()), &Xapvs);

	//MatrixXf Xdpvs(allclip.ClipFrames(), pvDim * dparts.size());
	//cache = dparts.replicate(1, 3).transpose();
	//selectCols(allclip.PvFacade.GetAllPartsSequence(), VectorXi::Map(cache.data(), cache.size()), &Xdpvs);

	Pca<MatrixXf> pcaXapvs(Xapvs);
	int dXapv = pcaXapvs.reducedRank(g_CharacterPcaCutoff);
	QrStore<MatrixXf> qrXabpvs(pcaXapvs.coordinates(dXapv));
	if (qrXabpvs.rank() == 0)
		return;

	Cca<float> cca;

	PcaCcaMap map;
	DrivenParts.clear();
	for (int i = 0; i < dparts.size(); i++)
	{
		auto pid = dparts[i];
		DrivenParts.emplace_back();
		auto &ctrl = DrivenParts.back();
		ctrl.DstIdx = pid;
		ctrl.SrcIdx = PvInputTypeEnum::ActiveParts;

		int d = allclip.PvFacade.GetPartPcaDim(pid);
		auto& pca = allclip.PvFacade.GetPartPca(pid);
		auto qr = allclip.PvFacade.GetPartPcadQrView(pid);

		cca.computeFromQr(qrXabpvs, qr, true, 0);
		float corr = cca.correlaltions().minCoeff();

		map.A = cca.matrixA();
		map.B = cca.matrixB();
		map.useInvB = false;
		map.svdBt = Eigen::JacobiSVD<Eigen::MatrixXf>(map.B.transpose(), Eigen::ComputeThinU | Eigen::ComputeThinV);
		map.uX = qrXabpvs.mean();
		map.uY = qr.mean();
		map.pcX = pcaXapvs.components(dXapv);
		map.uXpca = pcaXapvs.mean();
		map.pcY = pca.components(d);
		map.uYpca = pca.mean();

		ctrl.HomoMatrix = map.TransformMatrix();
	}
}

void PartilizedTransformer::EnableTracker(int whichTracker)
{
	m_currentTracker = whichTracker;
	m_useTracker = true;

	if (m_currentTracker >= m_Trackers.size() || m_currentTracker < 0)
	{
		m_currentTracker = -1;
		m_useTracker = false;
	}
}

void PartilizedTransformer::EnableTracker(const std::string & animName)
{
	auto& clips = m_controller.Character().Behavier().Clips();
	int which = std::find_if(BEGIN_TO_END(clips), [&animName](const ArmatureFrameAnimation& anim) { return anim.Name == animName;}) - clips.begin();
	EnableTracker(which);
}

void PartilizedTransformer::ResetTrackers()
{
	for (auto& tracker : m_Trackers)
		tracker.Reset();
}

RowVectorXf PartilizedTransformer::GetInputVector(const P2PTransform& Ctrl, const_frame_view source_frame, const_frame_view last_frame, float frame_time, int has_velocity) const
{
	const auto& sparts = *m_sParts;
	auto& feature = *m_pInputF;

	assert(sparts.Armature().size() == source_frame.size());

	int pvDim = feature.GetDimension();
	RowVectorXf Xd;

	if (Ctrl.SrcIdx >= 0)
	{
		auto& spart = *sparts[Ctrl.SrcIdx];
		pvDim = feature.GetDimension(spart);

		Xd.resize(has_velocity ? pvDim * 2 : pvDim);

		RowVectorXf xf = feature.Get(spart, source_frame);//,last_frame,frame_time

		TransformCtrlHandel(xf, Ctrl.HomoMatrix);

		Xd.segment(0, pvDim) = xf;

		if (has_velocity)
		{
			RowVectorXf xlf = feature.Get(spart, last_frame);
			TransformCtrlHandel(xlf, Ctrl.HomoMatrix);
			auto Xv = Xd.segment(pvDim, pvDim);
			Xv = (xf - xlf) / (frame_time * g_FrameTimeScaleFactor);
			if (has_velocity >= 2)
			{
				float norm = Xv.norm();
				if (norm > g_VelocityNormalizeThreshold)
					Xv /= norm;
			}

		}
	}
	else if (Ctrl.SrcIdx == PvInputTypeEnum::ActiveParts)
	{
		if (pvDim > 0)
		{
			if (has_velocity)
				pvDim *= 2;
			Xd.resize(pvDim * ActiveParts.size());
			for (int i = 0; i < ActiveParts.size(); i++)
			{
				auto& actrl = ActiveParts[i];
				Xd.segment(i * pvDim, pvDim) = GetInputVector(actrl, source_frame, last_frame, frame_time, has_velocity);
			}
		}
		else
		{
			throw std::logic_error("Input feature must be constant dimensional feature");
		}
	}

	return Xd;
	// TODO: insert return statement here
}

// The energy is compute in human-action space!
double PartilizedTransformer::GetInputKinectEnergy(_In_ const P2PTransform& Ctrl, _In_ const_frame_view source_frame, _In_ const_frame_view last_frame, _In_ double frame_time) const
{
	const auto& sparts = *m_sParts;
	auto& feature = *m_pInputF;
	assert(sparts.Armature().size() == source_frame.size());


	double energy = 0;
	if (Ctrl.SrcIdx >= 0)
	{
		auto& spart = *sparts[Ctrl.SrcIdx];
		RowVectorXf xf = feature.Get(spart, source_frame);//,last_frame,frame_time
		RowVectorXf xlf = feature.Get(spart, last_frame);

		xf = (xf - xlf) / frame_time;

		return xf.cwiseAbs2().sum();
	}
	else if (Ctrl.SrcIdx == PvInputTypeEnum::ActiveParts)
	{
		energy = 0;
		for (int i = 0; i < ActiveParts.size(); i++)
		{
			auto& actrl = ActiveParts[i];
			auto e = GetInputKinectEnergy(actrl, source_frame, last_frame, frame_time);
			energy += e;
			//energy = max(energy,e);
			// how to merge part energy?
		}
		energy /= ActiveParts.size();
	}

	return energy;
}

RowVectorXf PartilizedTransformer::GetCharacterInputVector(const P2PTransform& Ctrl, const_frame_view source_frame, const_frame_view last_frame, float frame_time, int has_velocity) const
{
	const auto& cparts = *m_cParts;
	auto& feature = *m_pInputF;

	assert(cparts.Armature().size() <= source_frame.size());

	int pvDim = feature.GetDimension();
	RowVectorXf Xd;

	if (Ctrl.DstIdx >= 0)
	{
		auto& cpart = *cparts[Ctrl.DstIdx];
		pvDim = feature.GetDimension(cpart);

		Xd.resize(has_velocity ? pvDim * 2 : pvDim);

		RowVectorXf xf = feature.Get(cpart, source_frame);//,last_frame,frame_time

		Xd.segment(0, pvDim) = xf;

		if (has_velocity)
		{
			RowVectorXf xlf = feature.Get(cpart, last_frame);
			auto Xv = Xd.segment(pvDim, pvDim);
			if (frame_time == .0f)
				Xv.setConstant(.0f);
			else
				Xv = (xf - xlf) / (frame_time * g_FrameTimeScaleFactor);
			if (has_velocity >= 2)
			{
				float norm = Xv.norm();
				if (norm > g_VelocityNormalizeThreshold)
					Xv /= norm;
			}
		}
	}
	else if (Ctrl.DstIdx == PvInputTypeEnum::ActiveParts)
	{
		if (pvDim > 0)
		{
			if (has_velocity)
				pvDim *= 2;
			Xd.resize(pvDim * ActiveParts.size());
			for (int i = 0; i < ActiveParts.size(); i++)
			{
				auto& actrl = ActiveParts[i];
				Xd.segment(i * pvDim, pvDim) = GetCharacterInputVector(actrl, source_frame, last_frame, frame_time, has_velocity);
			}
		}
		else
		{
			throw std::logic_error("Input feature must be constant dimensional feature");
		}
	}

	return Xd;
	// TODO: insert return statement here
}

void PartilizedTransformer::SetupTrackers(double expectedError, int stepSubdiv, double vtStep, double scaleStep, double vtStDev, double scaleStDev, double tInitDistSubdiv, int vtInitDistSubdiv, int scaleInitDistSubdiv)
{
	bool trackerVel = g_TrackerUseVelocity;
	auto pDeivce = m_controller.Character().Scene->GetRenderDevice();

	m_lowConfidentFrameCount = 0;
	m_lowConfidentTime = 0;
	auto& clips = m_controller.Character().Behavier().Clips();
	m_trackerConfidents.setZero(clips.size(),
		m_trackerSwitchTimeThreshold * MAX_FRAME_RATE);

	m_Trackers.reserve(clips.size());
	for (auto& anim : clips)
	{
		m_Trackers.emplace_back(anim, *this, pDeivce);
		auto& tracker = m_Trackers.back();
		tracker.SwitchAccelerater(g_TrackerGpuAcceleration ? CharacterActionTracker::GPU : CharacterActionTracker::SMP);
		//! HACK!!!
		int dim = ActiveParts.size() * m_pInputF->GetDimension();
		if (trackerVel)
			dim *= 2;
		Eigen::RowVectorXd var(dim);
		var.setConstant(expectedError);
		double velVar = g_TrackerNormalizedVelocityVariance;
		if (!g_NormalizeVelocity)
			velVar *= expectedError;

		if (trackerVel)
		{
			for (int i = 0, d = m_pInputF->GetDimension(); i < ActiveParts.size(); i++)
			{
				var.segment(i*d * 2 + d, d).setConstant(velVar);// *g_FrameTimeScaleFactor;
			}
		}

		tracker.SetLikihoodVarience(var.cast<IGestureTracker::ScalarType>());
		// dt = 1/30s, ds = 0.01, s = 0.3? 
		tracker.SetTrackingParameters(vtStep, sqr(vtStDev), scaleStep, sqr(scaleStDev));
		tracker.SetVelocityTolerance(g_TrackerVtThreshold);
		tracker.SetScaleTolerance(g_TrackerScaleThreshold);
		tracker.SetStepSubdivition(stepSubdiv);
		tracker.SetParticalesSubdiv(anim.GetFrameBuffer().size() * tInitDistSubdiv, scaleInitDistSubdiv, vtInitDistSubdiv);
		tracker.Reset();
	}
}

int GetFrameVectorSize(const IArmaturePartFeature* feature, const ShrinkedArmature& parts)
{
	int idx = 0;
	for (auto& part : parts)
	{
		idx += feature->GetDimension(*part);
	}
	return idx;

}

void PartilizedTransformer::DrivePartsTrackers(TrackerVectorType &_x, float frame_time, ArmatureFrameView target_frame)
{
	if (m_pTrackerFeature == nullptr)
	{
		auto spFeature = make_shared < RelativeDeformation <
			AllJoints < CharacterFeature > >> ();
		spFeature->SetDefaultFrame(m_tArmature->bind_frame());
		m_pTrackerFeature = spFeature;
	}

	auto& tarm = *m_tArmature;
	int bestTracker = m_currentTracker;

	auto confi = DrivePartsTracker(m_currentTracker, _x, frame_time, target_frame);

	double confidents[20];

	confidents[m_currentTracker] = confi;
	m_trackerConfidents(m_currentTracker, m_lowConfidentFrameCount) = confi;

	//cout << "confident = " << confi << endl;

	if (confi < m_trackerSwitchThreshold)
	{
		//Causality::Bone temp[100];
		ArmatureFrame temp(tarm.size());

		if (m_lowConfidentTime < m_trackerSwitchTimeThreshold
			|| m_lowConfidentFrameCount == m_trackerConfidents.cols())
		{
			for (int i = 0; i < m_Trackers.size(); i++)
			{
				if (i != m_currentTracker)
				{
					confidents[i] = DrivePartsTracker(i, _x, frame_time, temp);
					m_trackerConfidents(i, m_lowConfidentFrameCount) = confidents[i];
					//if (confidents[i] > confi)
					//{
					//	confi = confidents[i];
					//	bestTracker = i;
					//	std::copy_n(begin(temp), tarm.size(), target_frame.begin());
					//}
				}
			}

			m_lowConfidentTime += frame_time;
			++m_lowConfidentFrameCount;
		}
		else
		{
			m_lowConfidentTime = 0;
			m_lowConfidentFrameCount = 0;
			bestTracker = m_currentTracker;
			confi = m_trackerConfidents.leftCols(m_lowConfidentFrameCount)
				.rowwise().sum().maxCoeff(&bestTracker) / m_lowConfidentFrameCount;
			if (bestTracker != m_currentTracker && confi > m_trackerSwitchThreshold)
			{
				m_currentTracker = bestTracker;
				auto& clips = m_controller.Character().Behavier().Clips();
				if (m_currentTracker != -1)
					cout << "Switched to action [" << clips[m_currentTracker].Name << ']' << endl;
			}
		}
	}
	else
	{
		m_lowConfidentTime = 0;
		m_lowConfidentFrameCount = 0;
	}



	//RowVector3d ts;
	//ts.setZero();
	//tracker.GetScaledFrame(target_frame, ts[0], ts[1]);
	//cout << setw(6) << confi << " | " << ts << endl;

}


double PartilizedTransformer::DrivePartsTracker(int whichTracker, TrackerVectorType & _x, float frame_time, ArmatureFrameView target_frame)
{
	auto& cparts = *m_cParts;
	auto feature = m_pTrackerFeature.get();

	auto& tracker = m_Trackers[whichTracker];
	auto confi = tracker.Step(_x, frame_time);
	auto statue = tracker.CurrentState();

	auto dur = tracker.Animation().Duration.count();
	auto t = statue(0); auto s = statue(1);

	tracker.GetScaledFrame(target_frame, t, s);

	auto& subordinates = m_controller.GetCharacterSubordinates();
	for (auto& subchara : subordinates)
	{
		auto& subframe = subchara.Character->MapCurrentFrameForUpdate();

		double ts = t * subchara.SpeedPreference + subchara.PhasePreference * dur;
	
		tracker.GetScaledFrame(subframe,
			subchara.TimeFilter.Apply(ts),
			subchara.ScaleFilter.Apply(s * subchara.ScalePreference));
		subchara.Character->ReleaseCurrentFrameFrorUpdate();
	}

	return confi;

	auto* idcies = tracker.GetTopKStates(g_TrackerTopK);
	auto& sample = tracker.GetSampleMatrix();
	auto& liks = tracker.GetSampleLikilihoods();
	confi = 0;

	auto timeline = sample.col(0);
	auto scales = sample.col(1);

	auto fvsize = GetFrameVectorSize(feature, cparts);
	MatrixXf fmat(fvsize, g_TrackerTopK);
	for (int i = 0; i < g_TrackerTopK; i++)
	{
		int id = idcies[i];
		tracker.GetScaledFrame(target_frame, timeline(id), scales(id));
		GetFrameVector(feature, fmat.col(i), target_frame, cparts);
		confi += liks(id);
	}

	for (int i = 0; i < g_TrackerTopK; i++)
	{
		int id = idcies[i];
		fmat.col(i) *= (liks(id) / confi); // in order to prevent overflow, we do not multiply the weight until we get the normalization factor
	}

	auto fv = (fmat.rowwise().sum()).eval();

	SetFrameVector(feature, fv, target_frame, cparts);

	return confi / g_TrackerTopK;
}