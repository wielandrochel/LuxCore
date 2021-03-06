/***************************************************************************
 * Copyright 1998-2018 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <algorithm>

#if defined(_OPENMP)
#include <omp.h>
#endif

#include <boost/format.hpp>
#include <boost/circular_buffer.hpp>

#include "luxrays/core/geometry/bbox.h"
#include "slg/samplers/sobol.h"
#include "slg/lights/strategies/dlscacheimpl.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// DLSCOctree
//------------------------------------------------------------------------------

namespace slg {

class DLSCOctree {
public:
	DLSCOctree(const BBox &bbox, const float r, const float normAngle, const u_int md = 24) :
		worldBBox(bbox), maxDepth(md), entryRadius(r), entryRadius2(r * r),
		entryNormalCosAngle(cosf(Radians(normAngle))) {
		worldBBox.Expand(MachineEpsilon::E(worldBBox));
	}
	~DLSCOctree() {
		for (auto entry : allEntries)
			delete entry;
	}

	void Add(DLSCacheEntry *cacheEntry) {
		allEntries.push_back(cacheEntry);
		
		const Vector entryRadiusVector(entryRadius, entryRadius, entryRadius);
		const BBox entryBBox(cacheEntry->p - entryRadiusVector, cacheEntry->p + entryRadiusVector);

		AddImpl(&root, worldBBox, cacheEntry, entryBBox, DistanceSquared(entryBBox.pMin,  entryBBox.pMax));
	}

	vector<DLSCacheEntry *> &GetAllEntries() {
		return allEntries;
	}

	const DLSCacheEntry *GetEntry(const Point &p, const Normal &n,
			const bool isVolume) const {
		return GetEntryImpl(&root, worldBBox, p, n, isVolume);
	}
	
	void GetAllNearEntries(vector<DLSCacheEntry *> &entries,
			const Point &p, const Normal &n,
			const bool isVolume,
			const float radius) const {
		const Vector radiusVector(radius, radius, radius);
		const BBox bbox(p - radiusVector, p + radiusVector);

		return GetAllNearEntriesImpl(entries, &root, worldBBox,
				p, n, isVolume,
				bbox, radius * radius);
	}

	void DebugExport(const string &fileName, const float sphereRadius) const {
		Properties prop;

		prop <<
				Property("scene.materials.octree_material.type")("matte") <<
				Property("scene.materials.octree_material.kd")("0.75 0.75 0.75") <<
				Property("scene.materials.octree_material_red.type")("matte") <<
				Property("scene.materials.octree_material_red.kd")("0.75 0.0 0.0") <<
				Property("scene.materials.octree_material_red.emission")("0.25 0.0 0.0");

		for (u_int i = 0; i < allEntries.size(); ++i) {
			const DLSCacheEntry &entry = *(allEntries[i]);
			if (entry.IsDirectLightSamplingDisabled())
				prop << Property("scene.objects.octree_entry_" + ToString(i) + ".material")("octree_material_red");
			else
				prop << Property("scene.objects.octree_entry_" + ToString(i) + ".material")("octree_material");

			prop <<
				Property("scene.objects.octree_entry_" + ToString(i) + ".ply")("scenes/simple/sphere.ply") <<
				Property("scene.objects.octree_entry_" + ToString(i) + ".transformation")(Matrix4x4(
					sphereRadius, 0.f, 0.f, entry.p.x,
					0.f, sphereRadius, 0.f, entry.p.y,
					0.f, 0.f, sphereRadius, entry.p.z,
					0.f, 0.f, 0.f, 1.f));
		}

		prop.Save(fileName);
	}
	
private:
	class DLSCOctreeNode {
	public:
		DLSCOctreeNode() {
			for (u_int i = 0; i < 8; ++i)
				children[i] = NULL;
		}

		~DLSCOctreeNode() {
			for (u_int i = 0; i < 8; ++i)
				delete children[i];
		}

		DLSCOctreeNode *children[8];
		vector<DLSCacheEntry *> entries;
	};

	BBox ChildNodeBBox(u_int child, const BBox &nodeBBox,
		const Point &pMid) const {
		BBox childBound;

		childBound.pMin.x = (child & 0x4) ? pMid.x : nodeBBox.pMin.x;
		childBound.pMax.x = (child & 0x4) ? nodeBBox.pMax.x : pMid.x;
		childBound.pMin.y = (child & 0x2) ? pMid.y : nodeBBox.pMin.y;
		childBound.pMax.y = (child & 0x2) ? nodeBBox.pMax.y : pMid.y;
		childBound.pMin.z = (child & 0x1) ? pMid.z : nodeBBox.pMin.z;
		childBound.pMax.z = (child & 0x1) ? nodeBBox.pMax.z : pMid.z;

		return childBound;
	}

	void AddImpl(DLSCOctreeNode *node, const BBox &nodeBBox,
		DLSCacheEntry *entry, const BBox &entryBBox,
		const float entryBBoxDiagonal2, const u_int depth = 0) {
		// Check if I have to store the entry in this node
		if ((depth == maxDepth) ||
				DistanceSquared(nodeBBox.pMin, nodeBBox.pMax) < entryBBoxDiagonal2) {
			node->entries.push_back(entry);
			return;
		}

		// Determine which children the item overlaps
		const Point pMid = .5 * (nodeBBox.pMin + nodeBBox.pMax);

		const bool x[2] = {
			entryBBox.pMin.x <= pMid.x,
			entryBBox.pMax.x > pMid.x
		};
		const bool y[2] = {
			entryBBox.pMin.y <= pMid.y,
			entryBBox.pMax.y > pMid.y
		};
		const bool z[2] = {
			entryBBox.pMin.z <= pMid.z,
			entryBBox.pMax.z > pMid.z
		};

		const bool overlap[8] = {
			bool(x[0] & y[0] & z[0]),
			bool(x[0] & y[0] & z[1]),
			bool(x[0] & y[1] & z[0]),
			bool(x[0] & y[1] & z[1]),
			bool(x[1] & y[0] & z[0]),
			bool(x[1] & y[0] & z[1]),
			bool(x[1] & y[1] & z[0]),
			bool(x[1] & y[1] & z[1])
		};

		for (u_int child = 0; child < 8; ++child) {
			if (!overlap[child])
				continue;

			// Allocated the child node if required
			if (!node->children[child])
				node->children[child] = new DLSCOctreeNode();

			// Add the entry to each overlapping child
			const BBox childBBox = ChildNodeBBox(child, nodeBBox, pMid);
			AddImpl(node->children[child], childBBox,
					entry, entryBBox, entryBBoxDiagonal2, depth + 1);
		}
	}

	const DLSCacheEntry *GetEntryImpl(const DLSCOctreeNode *node, const BBox &nodeBBox,
		const Point &p, const Normal &n, const bool isVolume) const {
		// Check if I'm inside the node bounding box
		if (!nodeBBox.Inside(p))
			return NULL;

		// Check every entry in this node
		for (auto entry : node->entries) {
			if ((DistanceSquared(p, entry->p) <= entryRadius2) &&
					(isVolume == entry->isVolume) && 
					(isVolume || (Dot(n, entry->n) >= entryNormalCosAngle))) {
				// I have found a valid entry
				return entry;
			}
		}
		
		// Check the children too
		const Point pMid = .5 * (nodeBBox.pMin + nodeBBox.pMax);
		for (u_int child = 0; child < 8; ++child) {
			if (node->children[child]) {
				const BBox childBBox = ChildNodeBBox(child, nodeBBox, pMid);

				const DLSCacheEntry *entry = GetEntryImpl(node->children[child], childBBox,
						p, n, isVolume);
				if (entry) {
					// I have found a valid entry
					return entry;
				}
			}
		}
		
		return NULL;
	}

	void GetAllNearEntriesImpl(vector<DLSCacheEntry *> &entries,
			const DLSCOctreeNode *node, const BBox &nodeBBox,
			const Point &p, const Normal &n,
			const bool isVolume,
			const BBox areaBBox,
			const float areaRadius2) const {
		// Check if I overlap the node bounding box
		if (!nodeBBox.Overlaps(areaBBox))
			return;
		
		// Check every entry in this node
		for (auto entry : node->entries) {
			if ((DistanceSquared(p, entry->p) <= areaRadius2) &&
					(isVolume == entry->isVolume) && 
					(isVolume || (Dot(n, entry->n) >= entryNormalCosAngle))) {
				// I have found a valid entry but I avoid to insert duplicates
				if (find(entries.begin(), entries.end(), entry) == entries.end())
					entries.push_back(entry);
			}
		}
		
		// Check the children too
		const Point pMid = .5 * (nodeBBox.pMin + nodeBBox.pMax);
		for (u_int child = 0; child < 8; ++child) {
			if (node->children[child]) {
				const BBox childBBox = ChildNodeBBox(child, nodeBBox, pMid);

				GetAllNearEntriesImpl(entries, node->children[child], childBBox,
						p, n, isVolume,
						areaBBox, areaRadius2);
			}
		}
	}

	BBox worldBBox;
	
	u_int maxDepth;
	float entryRadius, entryRadius2, entryNormalCosAngle;
	
	DLSCOctreeNode root;
	vector<DLSCacheEntry *> allEntries;
};

}

//------------------------------------------------------------------------------
// Direct light sampling cache
//------------------------------------------------------------------------------

DirectLightSamplingCache::DirectLightSamplingCache() {
	maxSampleCount = 10000000;
	maxDepth = 4;
	maxEntryPasses = 1024;
	targetCacheHitRate = .995f;
	lightThreshold = .01f;

	entryRadius = .15f;
	entryNormalAngle = 10.f;
	entryConvergenceThreshold = .01f;
	entryWarmUpSamples = 12;

	entryOnVolumes = false;

	octree = NULL;
}

DirectLightSamplingCache::~DirectLightSamplingCache() {
	delete octree;
}

void DirectLightSamplingCache::GenerateEyeRay(const Camera *camera, Ray &eyeRay,
		PathVolumeInfo &volInfo, Sampler *sampler, SampleResult &sampleResult) const {
	const u_int *subRegion = camera->filmSubRegion;
	sampleResult.filmX = subRegion[0] + sampler->GetSample(0) * (subRegion[1] - subRegion[0] + 1);
	sampleResult.filmY = subRegion[2] + sampler->GetSample(1) * (subRegion[3] - subRegion[2] + 1);

	camera->GenerateRay(sampleResult.filmX, sampleResult.filmY, &eyeRay, &volInfo,
		sampler->GetSample(2), sampler->GetSample(3), sampler->GetSample(4));
}

void DirectLightSamplingCache::BuildCacheEntries(const Scene *scene) {
	SLG_LOG("Building direct light sampling cache: building cache entries");

	// Initialize the Octree where to store the cache points
	delete octree;
	octree = new DLSCOctree(scene->dataSet->GetBBox(), entryRadius, entryNormalAngle);
			
	// Initialize the sampler
	RandomGenerator rnd(131);
	SobolSamplerSharedData sharedData(&rnd, NULL);
	SobolSampler sampler(&rnd, NULL, NULL, 0.f, &sharedData);
	
	// Request the samples
	const u_int sampleBootSize = 5;
	const u_int sampleStepSize = 3;
	const u_int sampleSize = 
		sampleBootSize + // To generate eye ray
		maxDepth * sampleStepSize; // For each path vertex
	sampler.RequestSamples(sampleSize);
	
	// Initialize SampleResult 
	vector<SampleResult> sampleResults(1);
	SampleResult &sampleResult = sampleResults[0];
	sampleResult.Init(Film::RADIANCE_PER_PIXEL_NORMALIZED, 1);

	// Initialize the max. path depth
	PathDepthInfo maxPathDepth;
	maxPathDepth.depth = maxDepth;
	maxPathDepth.diffuseDepth = maxDepth;
	maxPathDepth.glossyDepth = maxDepth;
	maxPathDepth.specularDepth = maxDepth;

	double lastPrintTime = WallClockTime();
	u_int cacheLookUp = 0;
	u_int cacheHits = 0;
	double cacheHitRate = 0.0;
	bool cacheHitRateIsGood = false;
	for (u_int i = 0; i < maxSampleCount; ++i) {
		sampleResult.radiance[0] = Spectrum();
		
		Ray eyeRay;
		PathVolumeInfo volInfo;
		GenerateEyeRay(scene->camera, eyeRay, volInfo, &sampler, sampleResult);
		
		BSDFEvent lastBSDFEvent = SPECULAR;
		Spectrum pathThroughput(1.f);
		PathDepthInfo depthInfo;
		BSDF bsdf;
		for (;;) {
			sampleResult.firstPathVertex = (depthInfo.depth == 0);
			const u_int sampleOffset = sampleBootSize + depthInfo.depth * sampleStepSize;

			RayHit eyeRayHit;
			Spectrum connectionThroughput;
			const bool hit = scene->Intersect(NULL, false, sampleResult.firstPathVertex,
					&volInfo, sampler.GetSample(sampleOffset),
					&eyeRay, &eyeRayHit, &bsdf, &connectionThroughput,
					&pathThroughput, &sampleResult);
			pathThroughput *= connectionThroughput;
			// Note: pass-through check is done inside Scene::Intersect()

			if (!hit) {
				// Nothing was hit, time to stop
				break;
			}

			//------------------------------------------------------------------
			// Something was hit
			//------------------------------------------------------------------

			// Check if I have to flip the normal
			const Normal surfaceGeometryNormal = bsdf.hitPoint.intoObject ? bsdf.hitPoint.geometryN : -bsdf.hitPoint.geometryN;

			if (!bsdf.IsDelta() && (entryOnVolumes || !bsdf.IsVolume())) {
				// Check if a cache entry is available for this point
				if (octree->GetEntry(bsdf.hitPoint.p, surfaceGeometryNormal, bsdf.IsVolume()))
					++cacheHits;
				else {
					DLSCacheEntry *entry = new DLSCacheEntry(bsdf.hitPoint.p,
							surfaceGeometryNormal, bsdf.IsVolume(), volInfo);
					octree->Add(entry);
				}
				++cacheLookUp;
			}

			//------------------------------------------------------------------
			// Build the next vertex path ray
			//------------------------------------------------------------------

			// Check if I reached the max. depth
			sampleResult.lastPathVertex = depthInfo.IsLastPathVertex(maxPathDepth, bsdf.GetEventTypes());
			if (sampleResult.lastPathVertex && !sampleResult.firstPathVertex)
				break;

			Vector sampledDir;
			float cosSampledDir, lastPdfW;
			const Spectrum bsdfSample = bsdf.Sample(&sampledDir,
						sampler.GetSample(sampleOffset + 1),
						sampler.GetSample(sampleOffset + 2),
						&lastPdfW, &cosSampledDir, &lastBSDFEvent);
			sampleResult.passThroughPath = false;

			assert (!bsdfSample.IsNaN() && !bsdfSample.IsInf());
			if (bsdfSample.Black())
				break;
			assert (!isnan(lastPdfW) && !isinf(lastPdfW));

			if (sampleResult.firstPathVertex)
				sampleResult.firstPathVertexEvent = lastBSDFEvent;

			// Increment path depth informations
			depthInfo.IncDepths(lastBSDFEvent);

			pathThroughput *= bsdfSample;
			assert (!pathThroughput.IsNaN() && !pathThroughput.IsInf());

			// Update volume information
			volInfo.Update(lastBSDFEvent, bsdf);

			eyeRay.Update(bsdf.hitPoint.p, surfaceGeometryNormal, sampledDir);
		}
		
		sampler.NextSample(sampleResults);

		//----------------------------------------------------------------------
		// Check if I have a cache hit rate high enough to stop
		//----------------------------------------------------------------------

		cacheHitRate = (100.0 * cacheHits) / cacheLookUp;
		if ((cacheLookUp > 1000) && (cacheHitRate > 100.0 * targetCacheHitRate)) {
			SLG_LOG("Direct light sampling cache hit is greater than: " << boost::str(boost::format("%.4f") % (100.0 * targetCacheHitRate)) << "%");
			cacheHitRateIsGood = true;
			break;
		}

		const double now = WallClockTime();
		if (now - lastPrintTime > 2.0) {
			SLG_LOG("Direct light sampling cache entries: " << i << "/" << maxSampleCount <<" (" << (u_int)((100.0 * i) / maxSampleCount) << "%)");
			SLG_LOG("Direct light sampling cache hits: " << cacheHits << "/" << cacheLookUp <<" (" << boost::str(boost::format("%.4f") % cacheHitRate) << "%)");
			lastPrintTime = now;
		}

#ifdef WIN32
		// Work around Windows bad scheduling
		boost::this_thread::yield();
#endif
	}

	if (!cacheHitRateIsGood)
		SLG_LOG("WARNING: direct light sampling cache hit rate is not good enough: " << boost::str(boost::format("%.4f") % cacheHitRate) << "%");
		
	SLG_LOG("Direct light sampling cache total entries: " << octree->GetAllEntries().size());
}

float DirectLightSamplingCache::SampleLight(const Scene *scene, DLSCacheEntry *entry,
		const LightSource *light, const u_int pass) const {
	const float u1 = RadicalInverse(pass, 3);
	const float u2 = RadicalInverse(pass, 5);
	const float u3 = RadicalInverse(pass, 7);

	Vector lightRayDir;
	float distance, directPdfW;
	Spectrum lightRadiance = light->Illuminate(*scene, entry->p,
			u1, u2, u3, &lightRayDir, &distance, &directPdfW);
	assert (!lightRadiance.IsNaN() && !lightRadiance.IsInf());

	const float dotLight = Dot(lightRayDir, entry->n);
	if (!lightRadiance.Black() && (dotLight > 0.f)) {
		assert (!isnan(directPdfW) && !isinf(directPdfW));

		const float time = RadicalInverse(pass, 11);
		const float u4 = RadicalInverse(pass, 13);

		Ray shadowRay(entry->p, lightRayDir,
				0.f,
				distance,
				time);
		shadowRay.UpdateMinMaxWithEpsilon();
		RayHit shadowRayHit;
		BSDF shadowBsdf;
		Spectrum connectionThroughput;

		// Check if the light source is visible
		if (!scene->Intersect(NULL, false, false, &(entry->tmpInfo->volInfo), u4, &shadowRay,
				&shadowRayHit, &shadowBsdf, &connectionThroughput)) {
			// It is
			const Spectrum receivedRadiance = dotLight * connectionThroughput * lightRadiance;

			return receivedRadiance.Y();
		}
	}
	
	return 0.f;
}

void DirectLightSamplingCache::FillCacheEntry(const Scene *scene, DLSCacheEntry *entry) {
	const vector<LightSource *> &lights = scene->lightDefs.GetLightSources();

	vector<float> entryReceivedLuminance(lights.size(), 0.f);
	float maxLuminanceValue = 0.f;

	for (u_int lightIndex = 0; lightIndex < lights.size(); ++lightIndex) {
		const LightSource *light = lights[lightIndex];
	
		// Check if I can avoid to trace all shadow rays
		if (light->IsAlwaysInShadow(*scene, entry->p, entry->n)) {
			// It is always in shadow
			continue;
		}
		
		float receivedLuminance = 0.f;
		boost::circular_buffer<float> entryReceivedLuminancePreviousStep(entryWarmUpSamples, 0.f);

		u_int pass = 0;
		for (; pass < maxEntryPasses; ++pass) {
			receivedLuminance += SampleLight(scene, entry, light, pass);
			
			const float currentStepValue = receivedLuminance / pass;

			if (pass > entryWarmUpSamples) {
				// Convergence test, check if it is time to stop sampling
				// this light source. Using an 1% threshold.

				const float previousStepValue = entryReceivedLuminancePreviousStep[0];
				const float convergence = fabsf(currentStepValue - previousStepValue);

				const float threshold =  currentStepValue * entryConvergenceThreshold;

				if ((convergence == 0.f) || (convergence < threshold)) {
					// Done
					break;
				}
			}

			entryReceivedLuminancePreviousStep.push_back(currentStepValue);
		
#ifdef WIN32
			// Work around Windows bad scheduling
			boost::this_thread::yield();
#endif
		}
		
		entryReceivedLuminance[lightIndex] = receivedLuminance / pass;
		maxLuminanceValue = Max(maxLuminanceValue, entryReceivedLuminance[lightIndex]);

		// For some Debugging
		//SLG_LOG("Light #" << lightIndex << ": " << entryReceivedLuminance[lightIndex] <<	" (pass " << pass << ")");
	}
	
	if (maxLuminanceValue > 0.f) {
		// Use the higher light luminance to establish a threshold. Using an 1%
		// threshold at the moment.
		const float luminanceThreshold = maxLuminanceValue * lightThreshold;

		for (u_int lightIndex = 0; lightIndex < lights.size(); ++lightIndex) {
			if (entryReceivedLuminance[lightIndex] > luminanceThreshold) {
				// Add this light
				entry->tmpInfo->lightReceivedLuminance.push_back(entryReceivedLuminance[lightIndex]);
				entry->tmpInfo->distributionIndexToLightIndex.push_back(lightIndex);
			}
		}

		entry->tmpInfo->lightReceivedLuminance.shrink_to_fit();
		entry->tmpInfo->distributionIndexToLightIndex.shrink_to_fit();
	}

	/*SLG_LOG("===================================================================");
	for (u_int i = 0; i < entry->tmpInfo->lightReceivedLuminance.size(); ++i) {
		SLG_LOG("Light #" << entry->tmpInfo->distributionIndexToLightIndex[i] << ": " <<
				entry->tmpInfo->lightReceivedLuminance[i]);
	}
	SLG_LOG("===================================================================");*/
}

void DirectLightSamplingCache::FillCacheEntries(const Scene *scene) {
	SLG_LOG("Building direct light sampling cache: filling cache entries with " << scene->lightDefs.GetSize() << " light sources");

	vector<DLSCacheEntry *> &entries = octree->GetAllEntries();

	double lastPrintTime = WallClockTime();
	atomic<u_int> counter(0);
	
	#pragma omp parallel for
	for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int i = 0; i < entries.size(); ++i) {
		const int tid =
#if defined(_OPENMP)
			omp_get_thread_num()
#else
			0
#endif
			;

		if (tid == 0) {
			const double now = WallClockTime();
			if (now - lastPrintTime > 2.0) {
				SLG_LOG("Direct light sampling cache filled entries: " << counter << "/" << entries.size() <<" (" << (u_int)((100.0 * counter) / entries.size()) << "%)");
				lastPrintTime = now;
			}
		}
		
		FillCacheEntry(scene, entries[i]);
		
		++counter;
	}
}

void DirectLightSamplingCache::MergeCacheEntry(const Scene *scene, DLSCacheEntry *entry) {
	// Copy the temporary information
	entry->tmpInfo->mergedLightReceivedLuminance = entry->tmpInfo->lightReceivedLuminance;
	entry->tmpInfo->mergedDistributionIndexToLightIndex = entry->tmpInfo->distributionIndexToLightIndex;

	// Look for all near cache entries
	vector<DLSCacheEntry *> nearEntries;
	octree->GetAllNearEntries(nearEntries, entry->p, entry->n, entry->isVolume,
			entryRadius * 2.f);

	// Merge all found entries
	for (auto nearEntry : nearEntries) {
		// Avoid to merge with myself
		if (nearEntry == entry)
			continue;

		for (u_int i = 0; i < nearEntry->tmpInfo->distributionIndexToLightIndex.size(); ++i) {
			const u_int lightIndex = nearEntry->tmpInfo->distributionIndexToLightIndex[i];
			
			// Check if I have already this light
			bool found = false;
			for (u_int j = 0; j < entry->tmpInfo->mergedDistributionIndexToLightIndex.size(); ++j) {
				if (lightIndex == entry->tmpInfo->mergedDistributionIndexToLightIndex[j]) {
					// It is a light source I already have
					
					entry->tmpInfo->mergedLightReceivedLuminance[j] = (entry->tmpInfo->mergedLightReceivedLuminance[j] +
							nearEntry->tmpInfo->lightReceivedLuminance[i]) * .5f;
					found = true;
					break;
				}
			}
			
			if (!found) {
				// It is a new light
				entry->tmpInfo->mergedLightReceivedLuminance.push_back(nearEntry->tmpInfo->lightReceivedLuminance[i]);
				entry->tmpInfo->mergedDistributionIndexToLightIndex.push_back(lightIndex);
			}
		}
	}

	// Initialize the distribution
	if (entry->tmpInfo->mergedLightReceivedLuminance.size() > 0) {
		entry->lightsDistribution = new Distribution1D(&(entry->tmpInfo->mergedLightReceivedLuminance[0]),
				entry->tmpInfo->mergedLightReceivedLuminance.size());
		
		entry->distributionIndexToLightIndex = entry->tmpInfo->mergedDistributionIndexToLightIndex;
		entry->distributionIndexToLightIndex.shrink_to_fit();
	}
}

void DirectLightSamplingCache::MergeCacheEntries(const Scene *scene) {
	SLG_LOG("Building direct light sampling cache: merging cache entries");

	vector<DLSCacheEntry *> &entries = octree->GetAllEntries();

	double lastPrintTime = WallClockTime();
	atomic<u_int> counter(0);

	#pragma omp parallel for
	for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int i = 0; i < entries.size(); ++i) {
		const int tid =
#if defined(_OPENMP)
			omp_get_thread_num()
#else
			0
#endif
			;

		if (tid == 0) {
			const double now = WallClockTime();
			if (now - lastPrintTime > 2.0) {
				SLG_LOG("Direct light sampling cache merged entries: " << counter << "/" << entries.size() <<" (" << (u_int)((100.0 * counter) / entries.size()) << "%)");
				lastPrintTime = now;
			}
		}
		
		MergeCacheEntry(scene, entries[i]);
		
		++counter;
	}
}

void DirectLightSamplingCache::Build(const Scene *scene) {
	// This check is required because FILESAVER engine doesn't
	// initialize any accelerator
	if (!scene->dataSet->GetAccelerator()) {
		SLG_LOG("Direct light sampling cache is not built");
		return;
	}
	
	SLG_LOG("Building direct light sampling cache");

	BuildCacheEntries(scene);
	FillCacheEntries(scene);
	MergeCacheEntries(scene);

	// Delete all temporary information
	vector<DLSCacheEntry *> &entries = octree->GetAllEntries();
	for (auto entry : entries)
		entry->DeleteTmpInfo();
	
	// Export the otcree for debugging
	//octree->DebugExport("octree-point.scn", entryRadius * .05f);
}

const DLSCacheEntry *DirectLightSamplingCache::GetEntry(const luxrays::Point &p,
		const luxrays::Normal &n, const bool isVolume) const {
	if (!octree || (isVolume && !entryOnVolumes))
		return NULL;

	return octree->GetEntry(p, n, isVolume);
}
