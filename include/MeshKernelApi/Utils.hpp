//---- GPL ---------------------------------------------------------------------
//
// Copyright (C)  Stichting Deltares, 2011-2021.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 3.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// contact: delft3d.support@deltares.nl
// Stichting Deltares
// P.O. Box 177
// 2600 MH Delft, The Netherlands
//
// All indications and logos of, and references to, "Delft3D" and "Deltares"
// are registered trademarks of Stichting Deltares, and remain the property of
// Stichting Deltares. All rights reserved.
//
//------------------------------------------------------------------------------

#pragma once

#include <MeshKernel/AveragingInterpolation.hpp>
#include <MeshKernel/Constants.hpp>
#include <MeshKernel/CurvilinearGrid.hpp>
#include <MeshKernel/Entities.hpp>
#include <MeshKernel/Exceptions.hpp>
#include <MeshKernel/FlipEdges.hpp>
#include <MeshKernel/LandBoundaries.hpp>
#include <MeshKernel/Mesh2D.hpp>
#include <MeshKernel/MeshRefinement.hpp>
#include <MeshKernel/Operations.hpp>
#include <MeshKernel/Splines.hpp>

#include <MeshKernelApi/CurvilinearParameters.hpp>
#include <MeshKernelApi/GeometryList.hpp>
#include <MeshKernelApi/InterpolationParameters.hpp>
#include <MeshKernelApi/MakeMeshParameters.hpp>
#include <MeshKernelApi/MeshGeometry.hpp>
#include <MeshKernelApi/MeshGeometryDimensions.hpp>
#include <MeshKernelApi/OrthogonalizationParameters.hpp>
#include <MeshKernelApi/SampleRefineParameters.hpp>
#include <MeshKernelApi/SplinesToCurvilinearParameters.hpp>

#include <stdexcept>
#include <vector>

namespace meshkernelapi
{

    // TODO: Return result instead of relying on second input parameter
    static void ConvertGeometryListToPointVector(const GeometryList& geometryListIn, std::vector<meshkernel::Point>& result)
    {
        if (geometryListIn.numberOfCoordinates == 0)
        {
            return;
        }
        result.resize(geometryListIn.numberOfCoordinates);

        for (auto i = 0; i < geometryListIn.numberOfCoordinates; i++)
        {
            result[i] = {geometryListIn.xCoordinates[i], geometryListIn.yCoordinates[i]};
        }
    }

    // TODO: Return result instead of relying on second input parameter
    static void ConvertGeometryListToSampleVector(const GeometryList& geometryListIn, std::vector<meshkernel::Sample>& result)
    {
        if (geometryListIn.numberOfCoordinates == 0)
        {
            throw std::invalid_argument("MeshKernel: The samples are empty.");
        }
        result.resize(geometryListIn.numberOfCoordinates);

        for (auto i = 0; i < geometryListIn.numberOfCoordinates; i++)
        {
            result[i] = {geometryListIn.xCoordinates[i], geometryListIn.yCoordinates[i], geometryListIn.zCoordinates[i]};
        }
    }

    // TODO: Return result instead of relying on second input parameter
    static void ConvertPointVectorToGeometryList(std::vector<meshkernel::Point> pointVector, GeometryList& result)
    {
        if (pointVector.size() < result.numberOfCoordinates)
        {
            throw std::invalid_argument("MeshKernel: Invalid memory allocation, the point-vector size is smaller than the number of coordinates.");
        }

        for (auto i = 0; i < result.numberOfCoordinates; i++)
        {
            result.xCoordinates[i] = pointVector[i].x;
            result.yCoordinates[i] = pointVector[i].y;
        }
    }

    static bool SetSplines(const GeometryList& geometryListIn, meshkernel::Splines& spline)
    {
        if (geometryListIn.numberOfCoordinates == 0)
        {
            return false;
        }

        std::vector<meshkernel::Point> splineCornerPoints;
        ConvertGeometryListToPointVector(geometryListIn, splineCornerPoints);

        const auto indices = FindIndices(splineCornerPoints, 0, splineCornerPoints.size(), meshkernel::doubleMissingValue);

        for (const auto& index : indices)
        {
            const auto size = index[1] - index[0] + 1;
            if (size > 0)
            {
                spline.AddSpline(splineCornerPoints, index[0], size);
            }
        }

        return true;
    }

    static bool SetMeshGeometry(int meshKernelId, MeshGeometryDimensions& meshGeometryDimensions, MeshGeometry& meshGeometry, std::vector<std::shared_ptr<meshkernel::Mesh2D>> meshInstances)
    {
        if (meshKernelId >= meshInstances.size())
        {
            return false;
        }

        meshGeometry.nodex = &(meshInstances[meshKernelId]->m_nodex[0]);
        meshGeometry.nodey = &(meshInstances[meshKernelId]->m_nodey[0]);
        meshGeometry.nodez = &(meshInstances[meshKernelId]->m_nodez[0]);
        meshGeometry.edge_nodes = &(meshInstances[meshKernelId]->m_edgeNodes[0]);

        meshGeometryDimensions.maxnumfacenodes = meshkernel::maximumNumberOfNodesPerFace;
        meshGeometryDimensions.numface = static_cast<int>(meshInstances[meshKernelId]->GetNumFaces());
        if (meshGeometryDimensions.numface > 0)
        {
            meshGeometry.face_nodes = &(meshInstances[meshKernelId]->m_faceNodes[0]);
            meshGeometry.facex = &(meshInstances[meshKernelId]->m_facesCircumcentersx[0]);
            meshGeometry.facey = &(meshInstances[meshKernelId]->m_facesCircumcentersy[0]);
            meshGeometry.facez = &(meshInstances[meshKernelId]->m_facesCircumcentersz[0]);
        }

        if (meshInstances[meshKernelId]->GetNumNodes() == 1)
        {
            meshGeometryDimensions.numnode = 0;
            meshGeometryDimensions.numedge = 0;
        }
        else
        {
            meshGeometryDimensions.numnode = static_cast<int>(meshInstances[meshKernelId]->GetNumNodes());
            meshGeometryDimensions.numedge = static_cast<int>(meshInstances[meshKernelId]->GetNumEdges());
        }

        return true;
    }

    static std::vector<meshkernel::Point> ComputeLocations(const MeshGeometryDimensions& meshGeometryDimensions, const MeshGeometry& meshGeometry, meshkernel::MeshLocations interpolationLocation)
    {
        std::vector<meshkernel::Point> locations;
        if (interpolationLocation == meshkernel::MeshLocations::Nodes)
        {
            locations = meshkernel::ConvertToNodesVector(meshGeometryDimensions.numnode, meshGeometry.nodex, meshGeometry.nodey);
        }
        if (interpolationLocation == meshkernel::MeshLocations::Edges)
        {
            const auto edges = meshkernel::ConvertToEdgeNodesVector(meshGeometryDimensions.numedge, meshGeometry.edge_nodes);
            const auto nodes = meshkernel::ConvertToNodesVector(meshGeometryDimensions.numnode, meshGeometry.nodex, meshGeometry.nodey);
            locations = ComputeEdgeCenters(nodes, edges);
        }
        if (interpolationLocation == meshkernel::MeshLocations::Faces)
        {
            locations = meshkernel::ConvertToFaceCentersVector(meshGeometryDimensions.numface, meshGeometry.facex, meshGeometry.facey);
        }

        return locations;
    }
} // namespace meshkernelapi
