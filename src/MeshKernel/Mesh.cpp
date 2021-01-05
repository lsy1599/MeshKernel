//---- GPL ---------------------------------------------------------------------
//
// Copyright (C)  Stichting Deltares, 2011-2020.
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

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <numeric>
#include <stdexcept>
#include <vector>

#include <MeshKernel/Constants.hpp>
#include <MeshKernel/CurvilinearGrid.hpp>
#include <MeshKernel/Entities.hpp>
#include <MeshKernel/Exceptions.hpp>
#include <MeshKernel/Mesh.hpp>
#include <MeshKernel/Operations.hpp>
#include <MeshKernel/Polygons.hpp>
#include <MeshKernel/SpatialTrees.hpp>
#include <MeshKernel/TriangulationWrapper.hpp>
#include <MeshKernelApi/MakeMeshParameters.hpp>

meshkernel::Mesh::Mesh(const std::vector<Edge>& edges,
                       const std::vector<Point>& nodes,
                       Projection projection,
                       AdministrationOptions administration) : m_nodes(nodes), m_edges(edges), m_projection(projection)
{

    Administrate(administration);

    //no polygon involved, so node mask is 1 everywhere
    m_nodeMask.resize(m_nodes.size());
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 1);
};

void meshkernel::Mesh::DeleteInvalidNodesAndEdges()
{

    // Mask nodes connected to valid edges
    std::vector<bool> connectedNodes(m_nodes.size(), false);
    size_t numInvalidEdges = 0;

    for (const auto& edge : m_edges)
    {
        auto const firstNode = edge.first;
        auto const secondNode = edge.second;

        if (firstNode == sizetMissingValue || secondNode == sizetMissingValue)
        {
            numInvalidEdges++;
            continue;
        }

        connectedNodes[firstNode] = true;
        connectedNodes[secondNode] = true;
    }

    // Count all invalid nodes (note: there might be nodes that are not connected to an edge)
    size_t numInvalidNodes = 0;
    for (auto n = 0; n < m_nodes.size(); ++n)
    {
        // invalidate nodes that are not connected
        if (!connectedNodes[n])
        {
            m_nodes[n] = {doubleMissingValue, doubleMissingValue};
        }

        if (!m_nodes[n].IsValid())
        {
            numInvalidNodes++;
        }
    }

    // If nothing to invalidate return
    if (numInvalidEdges == 0 && numInvalidNodes == 0)
    {
        m_numNodes = m_nodes.size();
        m_numEdges = m_edges.size();
        return;
    }

    // Flag invalid nodes
    std::vector<size_t> validNodesIndices(m_nodes.size());
    std::fill(validNodesIndices.begin(), validNodesIndices.end(), sizetMissingValue);
    size_t validIndex = 0;
    for (auto n = 0; n < m_nodes.size(); ++n)
    {
        if (m_nodes[n].IsValid())
        {
            validNodesIndices[n] = validIndex;
            validIndex++;
        }
    }

    // Flag invalid edges
    for (auto& edge : m_edges)
    {
        auto const firstNode = edge.first;
        auto const secondNode = edge.second;

        if (firstNode != sizetMissingValue && secondNode != sizetMissingValue && validNodesIndices[firstNode] != sizetMissingValue && validNodesIndices[secondNode] != sizetMissingValue)
        {
            edge.first = validNodesIndices[firstNode];
            edge.second = validNodesIndices[secondNode];
            continue;
        }

        edge.first = sizetMissingValue;
        edge.second = sizetMissingValue;
    }

    // Remove invalid nodes, without reducing capacity
    const auto endNodeVector = std::remove_if(m_nodes.begin(), m_nodes.end(), [](const Point& n) { return !n.IsValid(); });
    m_nodes.erase(endNodeVector, m_nodes.end());
    m_numNodes = m_nodes.size();

    // Remove invalid edges, without reducing capacity
    const auto endEdgeVector = std::remove_if(m_edges.begin(), m_edges.end(), [](const Edge& e) { return e.first == sizetMissingValue || e.second == sizetMissingValue; });
    m_edges.erase(endEdgeVector, m_edges.end());
    m_numEdges = m_edges.size();
}

void meshkernel::Mesh::Administrate(AdministrationOptions administrationOption)
{
    DeleteInvalidNodesAndEdges();

    if (m_nodesRTreeRequiresUpdate && !m_nodesRTree.Empty())
    {
        m_nodesRTree.BuildTree(m_nodes);
        m_nodesRTreeRequiresUpdate = false;
    }

    if (m_edgesRTreeRequiresUpdate && !m_edgesRTree.Empty())
    {
        ComputeEdgesCenters();
        m_edgesRTree.BuildTree(m_edgesCenters);
        m_edgesRTreeRequiresUpdate = false;
    }

    // return if there are no nodes or no edges
    if (m_numNodes == 0 || m_numEdges == 0)
    {
        return;
    }

    m_nodesEdges.resize(m_nodes.size());
    std::fill(m_nodesEdges.begin(), m_nodesEdges.end(), std::vector<size_t>(maximumNumberOfEdgesPerNode, sizetMissingValue));

    m_nodesNumEdges.resize(m_nodes.size());
    std::fill(m_nodesNumEdges.begin(), m_nodesNumEdges.end(), 0);

    NodeAdministration();

    for (auto n = 0; n < GetNumNodes(); n++)
    {
        SortEdgesInCounterClockWiseOrder(n);
    }

    if (administrationOption == AdministrationOptions::AdministrateMeshEdges)
    {
        return;
    }

    // face administration
    m_numFaces = 0;

    m_edgesNumFaces.resize(m_edges.size());
    std::fill(m_edgesNumFaces.begin(), m_edgesNumFaces.end(), 0);

    m_edgesFaces.resize(m_edges.size());
    std::fill(m_edgesFaces.begin(), m_edgesFaces.end(), std::vector<size_t>(2, sizetMissingValue));

    m_facesMassCenters.clear();
    m_faceArea.clear();
    m_facesNodes.clear();
    m_facesEdges.clear();
    m_facesCircumcenters.clear();

    m_facesMassCenters.reserve(m_numNodes);
    m_faceArea.reserve(m_numNodes);
    m_facesNodes.reserve(m_numNodes);
    m_facesEdges.reserve(m_numNodes);
    m_facesCircumcenters.reserve(m_numNodes);

    // find faces
    FindFaces();

    // find mesh circumcenters
    ComputeFaceCircumcentersMassCentersAndAreas();

    // classify node types
    ClassifyNodes();
}

meshkernel::Mesh::Mesh(const CurvilinearGrid& curvilinearGrid, Projection projection)
{
    if (curvilinearGrid.m_grid.empty())
    {
        throw std::invalid_argument("Mesh::Mesh: The curvilinear grid is empty.");
    }

    std::vector<Point> nodes(curvilinearGrid.m_grid.size() * curvilinearGrid.m_grid[0].size());
    std::vector<Edge> edges(curvilinearGrid.m_grid.size() * (curvilinearGrid.m_grid[0].size() - 1) + (curvilinearGrid.m_grid.size() - 1) * curvilinearGrid.m_grid[0].size());
    std::vector<std::vector<size_t>> indices(curvilinearGrid.m_grid.size(), std::vector<size_t>(curvilinearGrid.m_grid[0].size(), sizetMissingValue));

    size_t ind = 0;
    for (auto m = 0; m < curvilinearGrid.m_grid.size(); m++)
    {
        for (auto n = 0; n < curvilinearGrid.m_grid[0].size(); n++)
        {
            if (curvilinearGrid.m_grid[m][n].IsValid())
            {
                nodes[ind] = curvilinearGrid.m_grid[m][n];
                indices[m][n] = ind;
                ind++;
            }
        }
    }
    nodes.resize(ind);

    ind = 0;
    for (auto m = 0; m < curvilinearGrid.m_grid.size() - 1; m++)
    {
        for (auto n = 0; n < curvilinearGrid.m_grid[0].size(); n++)
        {
            if (indices[m][n] != sizetMissingValue && indices[m + 1][n] != sizetMissingValue)
            {
                edges[ind].first = indices[m][n];
                edges[ind].second = indices[m + 1][n];
                ind++;
            }
        }
    }

    for (auto m = 0; m < curvilinearGrid.m_grid.size(); m++)
    {
        for (auto n = 0; n < curvilinearGrid.m_grid[0].size() - 1; n++)
        {
            if (indices[m][n] != sizetMissingValue && indices[m][n + 1] != sizetMissingValue)
            {
                edges[ind].first = indices[m][n];
                edges[ind].second = indices[m][n + 1];
                ind++;
            }
        }
    }
    edges.resize(ind);

    m_nodesRTreeRequiresUpdate = true;
    m_edgesRTreeRequiresUpdate = true;

    *this = Mesh(edges, nodes, projection, AdministrationOptions::AdministrateMeshEdges);
}

meshkernel::Mesh::Mesh(const std::vector<Point>& inputNodes, const Polygons& polygons, Projection projection) : m_projection(projection)
{
    // compute triangulation
    TriangulationWrapper triangulationWrapper;
    const auto numberOfTriangles = inputNodes.size() * 6 + 10;
    triangulationWrapper.Compute(inputNodes,
                                 TriangulationWrapper::TriangulationOptions::TriangulatePointsAndGenerateFaces,
                                 0.0,
                                 numberOfTriangles);

    // For each triangle check
    // 1. Validity of its internal angles
    // 2. Is inside the polygon
    // If so we mark the edges and we add them m_edges
    std::vector<bool> edgeNodesFlag(triangulationWrapper.m_numEdges, false);
    for (auto i = 0; i < triangulationWrapper.m_numFaces; ++i)
    {
        const auto goodTriangle = CheckTriangle(triangulationWrapper.m_faceNodes[i], inputNodes);

        if (!goodTriangle)
        {
            continue;
        }
        const Point approximateCenter = (inputNodes[triangulationWrapper.m_faceNodes[i][0]] + inputNodes[triangulationWrapper.m_faceNodes[i][1]] + inputNodes[triangulationWrapper.m_faceNodes[i][2]]) * oneThird;

        const auto isTriangleInPolygon = polygons.IsPointInPolygon(approximateCenter, 0);
        if (!isTriangleInPolygon)
        {
            continue;
        }

        // mark all edges of this triangle as good ones
        for (auto j = 0; j < numNodesInTriangle; ++j)
        {
            edgeNodesFlag[triangulationWrapper.m_faceEdges[i][j]] = true;
        }
    }

    // now add all points and all valid edges
    m_nodes = inputNodes;
    size_t validEdgesCount = 0;
    for (auto i = 0; i < triangulationWrapper.m_numEdges; ++i)
    {
        if (!edgeNodesFlag[i])
            continue;
        validEdgesCount++;
    }

    std::vector<Edge> edges(validEdgesCount);
    validEdgesCount = 0;
    for (auto i = 0; i < triangulationWrapper.m_numEdges; ++i)
    {
        if (!edgeNodesFlag[i])
            continue;

        edges[validEdgesCount].first = triangulationWrapper.m_edgeNodes[i][0];
        edges[validEdgesCount].second = triangulationWrapper.m_edgeNodes[i][1];
        validEdgesCount++;
    }

    m_nodesRTreeRequiresUpdate = true;
    m_edgesRTreeRequiresUpdate = true;

    *this = Mesh(edges, inputNodes, projection, AdministrationOptions::AdministrateMeshEdges);
}

bool meshkernel::Mesh::CheckTriangle(const std::vector<size_t>& faceNodes, const std::vector<Point>& nodes) const
{
    // Used for triangular grids
    constexpr double triangleMinimumAngle = 5.0;
    constexpr double triangleMaximumAngle = 150.0;

    double phiMin = 1e3;
    double phiMax = 0.0;

    static std::array<std::array<size_t, 3>, 3> nodePermutations{{{2, 0, 1}, {0, 1, 2}, {1, 2, 0}}};

    for (auto i = 0; i < faceNodes.size(); ++i)
    {
        Point x0 = nodes[faceNodes[nodePermutations[i][0]]];
        Point x1 = nodes[faceNodes[nodePermutations[i][1]]];
        Point x2 = nodes[faceNodes[nodePermutations[i][2]]];

        const auto cosphi = NormalizedInnerProductTwoSegments(x1, x0, x1, x2, m_projection);
        const auto phi = std::acos(std::min(std::max(cosphi, -1.0), 1.0)) * raddeg_hp;
        phiMin = std::min(phiMin, phi);
        phiMax = std::max(phiMax, phi);
        if (phi < triangleMinimumAngle || phi > triangleMaximumAngle)
        {
            return false;
        }
    }
    return true;
}

void meshkernel::Mesh::SetFlatCopies(AdministrationOptions administrationOption)
{
    Administrate(administrationOption);

    m_nodex.resize(GetNumNodes());
    m_nodey.resize(GetNumNodes());
    m_nodez.resize(GetNumNodes());
    for (auto n = 0; n < GetNumNodes(); n++)
    {
        m_nodex[n] = m_nodes[n].x;
        m_nodey[n] = m_nodes[n].y;
        m_nodez[n] = 0.0;
    }

    size_t edgeIndex = 0;
    m_edgeNodes.resize(GetNumEdges() * 2);
    for (auto e = 0; e < GetNumEdges(); e++)
    {
        m_edgeNodes[edgeIndex] = static_cast<int>(m_edges[e].first);
        edgeIndex++;
        m_edgeNodes[edgeIndex] = static_cast<int>(m_edges[e].second);
        edgeIndex++;
    }

    m_faceNodes.resize(GetNumFaces() * maximumNumberOfNodesPerFace, intMissingValue);
    m_facesCircumcentersx.resize(GetNumFaces());
    m_facesCircumcentersy.resize(GetNumFaces());
    m_facesCircumcentersz.resize(GetNumFaces());
    size_t faceIndex = 0;
    for (auto f = 0; f < GetNumFaces(); f++)
    {
        for (auto n = 0; n < maximumNumberOfNodesPerFace; ++n)
        {
            if (n < m_facesNodes[f].size())
            {
                m_faceNodes[faceIndex] = static_cast<int>(m_facesNodes[f][n]);
            }
            faceIndex++;
        }
        m_facesCircumcentersx[f] = m_facesCircumcenters[f].x;
        m_facesCircumcentersy[f] = m_facesCircumcenters[f].y;
        m_facesCircumcentersz[f] = 0.0;
    }

    //we always need to provide pointers to not empty memory
    if (m_nodex.empty())
    {
        m_nodex.resize(1);
    }
    if (m_nodey.empty())
    {
        m_nodey.resize(1);
    }
    if (m_nodez.empty())
    {
        m_nodez.resize(1);
    }
    if (m_edgeNodes.empty())
    {
        m_edgeNodes.resize(1);
    }
    if (m_faceNodes.empty())
    {
        m_faceNodes.resize(1, intMissingValue);
    }
    if (m_facesCircumcentersx.empty())
    {
        m_facesCircumcentersx.resize(1);
    }
    if (m_facesCircumcentersy.empty())
    {
        m_facesCircumcentersy.resize(1);
    }
    if (m_facesCircumcentersz.empty())
    {
        m_facesCircumcentersz.resize(1);
    }
}

void meshkernel::Mesh::NodeAdministration()
{
    // assume no duplicated links
    for (auto e = 0; e < GetNumEdges(); e++)
    {
        const auto firstNode = m_edges[e].first;
        const auto secondNode = m_edges[e].second;

        if (firstNode == sizetMissingValue || secondNode == sizetMissingValue)
        {
            continue;
        }

        if (m_nodesNumEdges[firstNode] >= maximumNumberOfEdgesPerNode || m_nodesNumEdges[secondNode] >= maximumNumberOfEdgesPerNode)
        {
            continue;
        }

        // Search for previously connected edges
        auto alreadyAddedEdge = false;
        for (auto i = 0; i < m_nodesNumEdges[firstNode]; ++i)
        {
            const auto currentEdge = m_edges[m_nodesEdges[firstNode][i]];
            if (currentEdge.first == secondNode || currentEdge.second == secondNode)
            {
                alreadyAddedEdge = true;
                break;
            }
        }
        if (!alreadyAddedEdge)
        {
            m_nodesEdges[firstNode][m_nodesNumEdges[firstNode]] = e;
            m_nodesNumEdges[firstNode]++;
        }

        // Search for previously connected edges
        alreadyAddedEdge = false;
        for (auto i = 0; i < m_nodesNumEdges[secondNode]; ++i)
        {
            const auto currentEdge = m_edges[m_nodesEdges[secondNode][i]];
            if (currentEdge.first == firstNode || currentEdge.second == firstNode)
            {
                alreadyAddedEdge = true;
                break;
            }
        }
        if (!alreadyAddedEdge)
        {
            m_nodesEdges[secondNode][m_nodesNumEdges[secondNode]] = e;
            m_nodesNumEdges[secondNode]++;
        }
    }

    // resize
    for (auto n = 0; n < GetNumNodes(); n++)
    {
        m_nodesEdges[n].resize(m_nodesNumEdges[n]);
    }
};

void meshkernel::Mesh::SortEdgesInCounterClockWiseOrder(size_t node)
{
    if (!m_nodes[node].IsValid())
    {
        throw std::invalid_argument("Mesh::SortEdgesInCounterClockWiseOrder: Invalid nodes.");
    }

    double phi0 = 0.0;
    double phi;
    m_edgeAngles.resize(maximumNumberOfEdgesPerNode);
    std::fill(m_edgeAngles.begin(), m_edgeAngles.end(), 0.0);
    for (auto edgeIndex = 0; edgeIndex < m_nodesNumEdges[node]; edgeIndex++)
    {

        auto firstNode = m_edges[m_nodesEdges[node][edgeIndex]].first;
        auto secondNode = m_edges[m_nodesEdges[node][edgeIndex]].second;
        if (firstNode == sizetMissingValue || secondNode == sizetMissingValue)
        {
            continue;
        }

        if (secondNode == node)
        {
            secondNode = firstNode;
            firstNode = node;
        }

        const auto deltaX = GetDx(m_nodes[secondNode], m_nodes[firstNode], m_projection);
        const auto deltaY = GetDy(m_nodes[secondNode], m_nodes[firstNode], m_projection);
        if (abs(deltaX) < minimumDeltaCoordinate && abs(deltaY) < minimumDeltaCoordinate)
        {
            if (deltaY < 0.0)
            {
                phi = -M_PI / 2.0;
            }
            else
            {
                phi = M_PI / 2.0;
            }
        }
        else
        {
            phi = atan2(deltaY, deltaX);
        }

        if (edgeIndex == 0)
        {
            phi0 = phi;
        }

        m_edgeAngles[edgeIndex] = phi - phi0;
        if (m_edgeAngles[edgeIndex] < 0.0)
        {
            m_edgeAngles[edgeIndex] = m_edgeAngles[edgeIndex] + 2.0 * M_PI;
        }
    }

    // Performing sorting
    std::vector<std::size_t> indices(m_nodesNumEdges[node]);
    std::vector<size_t> edgeNodeCopy{m_nodesEdges[node]};
    iota(indices.begin(), indices.end(), 0);
    sort(indices.begin(), indices.end(), [&, this](std::size_t i1, std::size_t i2) { return m_edgeAngles[i1] < m_edgeAngles[i2]; });

    for (std::size_t edgeIndex = 0; edgeIndex < m_nodesNumEdges[node]; edgeIndex++)
    {
        m_nodesEdges[node][edgeIndex] = edgeNodeCopy[indices[edgeIndex]];
    }
}

void meshkernel::Mesh::DeleteDegeneratedTriangles()
{
    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);

    // assume the max amount of degenerated triangles is 10% of the actual faces
    std::vector<size_t> degeneratedTriangles;
    degeneratedTriangles.reserve(static_cast<size_t>(static_cast<double>(GetNumFaces()) * 0.1));
    for (auto f = 0; f < GetNumFaces(); ++f)
    {
        const auto numFaceNodes = m_numFacesNodes[f];
        if (numFaceNodes != numNodesInTriangle)
        {
            continue;
        }
        auto firstNode = m_facesNodes[f][0];
        auto secondNode = m_facesNodes[f][1];
        auto thirdNode = m_facesNodes[f][2];

        // account for periodic spherical coordinate
        if ((m_projection == Projection::spherical || m_projection == Projection::sphericalAccurate) && IsPointOnPole(m_nodes[firstNode]))
        {
            const auto saveFirstNode = firstNode;
            firstNode = secondNode;
            secondNode = thirdNode;
            thirdNode = saveFirstNode;
        }

        // compute coordinate differences, to check for collinearity
        const auto dx2 = GetDx(m_nodes[firstNode], m_nodes[secondNode], m_projection);
        const auto dy2 = GetDy(m_nodes[firstNode], m_nodes[secondNode], m_projection);
        const auto dx3 = GetDx(m_nodes[firstNode], m_nodes[thirdNode], m_projection);
        const auto dy3 = GetDy(m_nodes[firstNode], m_nodes[thirdNode], m_projection);

        const auto den = dy2 * dx3 - dy3 * dx2;

        if (IsEqual(den, 0.0))
        {
            // Flag edges to remove
            for (auto e = 0; e < numNodesInTriangle; ++e)
            {
                const auto edge = m_facesEdges[f][e];
                m_edges[edge] = {sizetMissingValue, sizetMissingValue};
            }
            // save degenerated face index
            degeneratedTriangles.emplace_back(f);
        }
    }

    // collapse secondNode and thirdNode into firstNode, change coordinate of the firstNode to triangle center of mass
    for (auto const& face : degeneratedTriangles)
    {
        const auto firstNode = m_facesNodes[face][0];
        const auto secondNode = m_facesNodes[face][1];
        const auto thirdNode = m_facesNodes[face][2];

        m_nodes[thirdNode] = m_facesMassCenters[face];
        MergeTwoNodes(secondNode, firstNode);
        MergeTwoNodes(thirdNode, firstNode);
    }

    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);
}

void meshkernel::Mesh::FindFacesRecursive(size_t startingNode,
                                          size_t node,
                                          size_t index,
                                          size_t previousEdge,
                                          size_t numClosingEdges,
                                          std::vector<size_t>& edges,
                                          std::vector<size_t>& nodes,
                                          std::vector<size_t>& sortedEdgesFaces,
                                          std::vector<size_t>& sortedNodes,
                                          std::vector<Point>& nodalValues)
{
    // The selected edge does not exist.
    // TODO: It would make to throw an exception here, but then the test cases fail
    if (index >= numClosingEdges)
        return;

    if (m_edges[previousEdge].first == sizetMissingValue || m_edges[previousEdge].second == sizetMissingValue)
        throw std::invalid_argument("Mesh::FindFacesRecursive: The selected edge is invalid. This should not happen since all invalid edges should have been cleaned up.");

    // Check if the faces are already found
    if (m_edgesNumFaces[previousEdge] >= 2)
        return;

    edges[index] = previousEdge;
    nodes[index] = node;
    const auto otherNode = OtherNodeOfEdge(m_edges[previousEdge], node);

    // enclosure found
    if (otherNode == startingNode && index == edges.size() - 1)
    {
        // no duplicated nodes allowed
        sortedNodes = nodes;
        std::sort(sortedNodes.begin(), sortedNodes.end());
        for (auto n = 0; n < sortedNodes.size() - 1; n++)
        {
            if (sortedNodes[n + 1] == sortedNodes[n])
            {
                return;
            }
        }

        // we need to add a face when at least one edge has no faces
        auto oneEdgeHasNoFace = false;
        for (const auto& edge : edges)
        {
            if (m_edgesNumFaces[edge] == 0)
            {
                oneEdgeHasNoFace = true;
                break;
            }
        }

        // check if least one edge has no face
        if (!oneEdgeHasNoFace)
        {
            // is an internal face only if all edges have a different face
            for (auto ee = 0; ee < edges.size(); ee++)
            {
                sortedEdgesFaces[ee] = m_edgesFaces[edges[ee]][0];
            }
            std::sort(sortedEdgesFaces.begin(), sortedEdgesFaces.end());
            for (auto n = 0; n < sortedEdgesFaces.size() - 1; n++)
            {
                if (sortedEdgesFaces[n + 1] == sortedEdgesFaces[n])
                    return;
            }
        }

        // the order of the edges in a new face must be counterclockwise
        // in order to evaluate the clockwise order, the signed face area is computed
        nodalValues.clear();
        for (const auto& node : nodes)
        {
            nodalValues.emplace_back(m_nodes[node]);
        }
        nodalValues.emplace_back(nodalValues.front());

        double area;
        Point centerOfMass;
        bool isCounterClockWise;
        FaceAreaAndCenterOfMass(nodalValues, m_projection, area, centerOfMass, isCounterClockWise);
        if (!isCounterClockWise)
        {
            return;
        }

        // increase m_edgesNumFaces
        m_numFaces += 1;
        for (const auto& edge : edges)
        {
            m_edgesNumFaces[edge] += 1;
            const auto numFace = m_edgesNumFaces[edge];
            m_edgesFaces[edge][numFace - 1] = m_numFaces - 1;
        }

        // store the result
        m_facesNodes.emplace_back(nodes);
        m_facesEdges.emplace_back(edges);
        m_faceArea.emplace_back(area);
        m_facesMassCenters.emplace_back(centerOfMass);

        return;
    }

    size_t edgeIndexOtherNode = 0;
    for (auto e = 0; e < m_nodesNumEdges[otherNode]; e++)
    {
        if (m_nodesEdges[otherNode][e] == previousEdge)
        {
            edgeIndexOtherNode = e;
            break;
        }
    }

    if (edgeIndexOtherNode == 0)
    {
        edgeIndexOtherNode = m_nodesNumEdges[otherNode] - 1;
    }
    else if (edgeIndexOtherNode > m_nodesNumEdges[otherNode])
    {
        edgeIndexOtherNode = edgeIndexOtherNode - m_nodesNumEdges[otherNode] - 1;
    }
    else
    {
        edgeIndexOtherNode = edgeIndexOtherNode - 1;
    }

    const auto edge = m_nodesEdges[otherNode][edgeIndexOtherNode];
    FindFacesRecursive(startingNode, otherNode, index + 1, edge, numClosingEdges, edges, nodes, sortedEdgesFaces, sortedNodes, nodalValues);
}

void meshkernel::Mesh::FindFaces()
{
    for (auto numEdgesPerFace = 3; numEdgesPerFace <= maximumNumberOfEdgesPerFace; numEdgesPerFace++)
    {
        std::vector<size_t> edges(numEdgesPerFace);
        std::vector<size_t> nodes(numEdgesPerFace);
        std::vector<size_t> sortedEdgesFaces(numEdgesPerFace);
        std::vector<size_t> sortedNodes(numEdgesPerFace);
        std::vector<Point> nodalValues;
        nodalValues.reserve(maximumNumberOfEdgesPerFace);
        for (auto n = 0; n < GetNumNodes(); n++)
        {
            if (!m_nodes[n].IsValid())
            {
                continue;
            }

            for (auto e = 0; e < m_nodesNumEdges[n]; e++)
            {
                FindFacesRecursive(n, n, 0, m_nodesEdges[n][e], numEdgesPerFace, edges, nodes, sortedEdgesFaces, sortedNodes, nodalValues);
            }
        }
    }

    m_numFacesNodes.resize(m_numFaces);
    for (auto f = 0; f < m_numFaces; ++f)
    {
        m_numFacesNodes[f] = m_facesNodes[f].size();
    }
}

void meshkernel::Mesh::ComputeFaceCircumcentersMassCentersAndAreas(bool computeMassCenters)
{
    m_facesCircumcenters.resize(GetNumFaces());
    m_faceArea.resize(GetNumFaces());
    m_facesMassCenters.resize(GetNumFaces());

    std::vector<size_t> numEdgeFacesCache;
    numEdgeFacesCache.reserve(maximumNumberOfEdgesPerFace);
    for (auto f = 0; f < GetNumFaces(); f++)
    {
        //need to account for spherical coordinates. Build a polygon around a face
        ComputeFaceClosedPolygon(f, m_polygonNodesCache);

        if (computeMassCenters)
        {
            double area;
            Point centerOfMass;
            bool isCounterClockWise;
            FaceAreaAndCenterOfMass(m_polygonNodesCache, m_projection, area, centerOfMass, isCounterClockWise);
            m_faceArea[f] = area;
            m_facesMassCenters[f] = centerOfMass;
        }

        size_t numberOfInteriorEdges = 0;
        const auto numberOfFaceNodes = GetNumFaceEdges(f);
        for (auto n = 0; n < numberOfFaceNodes; n++)
        {
            if (!IsEdgeOnBoundary(m_facesEdges[f][n]))
            {
                numberOfInteriorEdges += 1;
            }
        }
        if (numberOfInteriorEdges == 0)
        {
            m_facesCircumcenters[f] = m_facesMassCenters[f];
            continue;
        }
        numEdgeFacesCache.clear();
        for (auto n = 0; n < numberOfFaceNodes; n++)
        {
            numEdgeFacesCache.emplace_back(m_edgesNumFaces[m_facesEdges[f][n]]);
        }

        m_facesCircumcenters[f] = ComputeFaceCircumenter(m_polygonNodesCache,
                                                         numEdgeFacesCache);
    }
}

void meshkernel::Mesh::ClassifyNodes()
{
    m_nodesTypes.resize(GetNumNodes(), 0);
    std::fill(m_nodesTypes.begin(), m_nodesTypes.end(), 0);

    for (auto e = 0; e < GetNumEdges(); e++)
    {
        const auto firstNode = m_edges[e].first;
        const auto secondNode = m_edges[e].second;

        if (firstNode == sizetMissingValue || secondNode == sizetMissingValue)
        {
            continue;
        }

        if (m_nodesTypes[firstNode] == -1 || m_nodesTypes[secondNode] == -1)
        {
            continue;
        }

        if (m_edgesNumFaces[e] == 0)
        {
            m_nodesTypes[firstNode] = -1;
            m_nodesTypes[secondNode] = -1;
        }
        if (IsEdgeOnBoundary(e))
        {
            m_nodesTypes[firstNode] += 1;
            m_nodesTypes[secondNode] += 1;
        }
    }

    for (auto n = 0; n < GetNumNodes(); n++)
    {
        if (m_nodesTypes[n] == 1 || m_nodesTypes[n] == 2)
        {
            if (m_nodesNumEdges[n] == 2)
            {
                //corner point
                m_nodesTypes[n] = 3;
            }
            else
            {
                size_t firstNode = sizetMissingValue;
                size_t secondNode = sizetMissingValue;
                for (auto i = 0; i < m_nodesNumEdges[n]; i++)
                {
                    const auto edgeIndex = m_nodesEdges[n][i];
                    if (!IsEdgeOnBoundary(edgeIndex))
                    {
                        continue;
                    }
                    if (firstNode == 0)
                    {
                        firstNode = OtherNodeOfEdge(m_edges[edgeIndex], n);
                    }
                    else
                    {
                        secondNode = OtherNodeOfEdge(m_edges[edgeIndex], n);
                        break;
                    }
                }

                // point at the border
                m_nodesTypes[n] = 2;
                if (firstNode != sizetMissingValue && secondNode != sizetMissingValue)
                {
                    const double cosPhi = NormalizedInnerProductTwoSegments(m_nodes[n], m_nodes[firstNode], m_nodes[n], m_nodes[secondNode], m_projection);

                    // threshold for corner points
                    const double cornerCosine = 0.25;
                    if (cosPhi > -cornerCosine)
                    {
                        // void angle
                        m_nodesTypes[n] = 3;
                    }
                }
            }
        }
        else if (m_nodesTypes[n] > 2)
        {
            // corner point
            m_nodesTypes[n] = 3;
        }
        else if (m_nodesTypes[n] != -1)
        {
            //internal node
            m_nodesTypes[n] = 1;
        }
        if (m_nodesNumEdges[n] < 2)
        {
            //hanging node
            m_nodesTypes[n] = -1;
        }
    }
}

void meshkernel::Mesh::MakeMesh(const meshkernelapi::MakeMeshParameters& MakeMeshParameters, const Polygons& polygons)
{
    CurvilinearGrid CurvilinearGrid;
    m_projection = polygons.m_projection;
    if (MakeMeshParameters.GridType == 0)
    {
        // regular grid
        auto numM = static_cast<size_t>(MakeMeshParameters.NumberOfColumns + 1);
        auto numN = static_cast<size_t>(MakeMeshParameters.NumberOfRows + 1);
        const double XGridBlockSize = MakeMeshParameters.XGridBlockSize;
        const double YGridBlockSize = MakeMeshParameters.YGridBlockSize;
        const double cosineAngle = std::cos(MakeMeshParameters.GridAngle * degrad_hp);
        const double sinAngle = std::sin(MakeMeshParameters.GridAngle * degrad_hp);
        double OriginXCoordinate = MakeMeshParameters.OriginXCoordinate;
        double OriginYCoordinate = MakeMeshParameters.OriginYCoordinate;

        // in case a polygon is there, re-compute parameters
        if (!polygons.IsEmpty())
        {
            Point referencePoint{doubleMissingValue, doubleMissingValue};
            // rectangular grid in polygon

            for (const auto& node : polygons.m_nodes)
            {
                if (node.IsValid())
                {
                    referencePoint = node;
                    break;
                }
            }

            // get polygon min/max in rotated (xi,eta) coordinates
            double xmin = std::numeric_limits<double>::max();
            double xmax = -xmin;
            double etamin = std::numeric_limits<double>::max();
            double etamax = -etamin;
            for (const auto& node : polygons.m_nodes)
            {
                if (node.IsValid())
                {
                    const double dx = GetDx(referencePoint, node, m_projection);
                    const double dy = GetDy(referencePoint, node, m_projection);
                    double xi = dx * cosineAngle + dy * sinAngle;
                    double eta = -dx * sinAngle + dy * cosineAngle;
                    xmin = std::min(xmin, xi);
                    xmax = std::max(xmax, xi);
                    etamin = std::min(etamin, eta);
                    etamax = std::max(etamax, eta);
                }
            }

            double xShift = xmin * cosineAngle - etamin * sinAngle;
            double yShift = xmin * sinAngle + etamin * cosineAngle;
            if (m_projection == Projection::spherical)
            {
                xShift = xShift / earth_radius * raddeg_hp;
                yShift = yShift / (earth_radius * std::cos(referencePoint.y * degrad_hp)) * raddeg_hp;
            }

            OriginXCoordinate = referencePoint.x + xShift;
            OriginYCoordinate = referencePoint.y + yShift;
            numN = static_cast<size_t>(std::ceil((etamax - etamin) / XGridBlockSize) + 1);
            numM = static_cast<size_t>(std::ceil((xmax - xmin) / YGridBlockSize) + 1);
        }

        CurvilinearGrid = {numN, numM};
        for (auto n = 0; n < numN; ++n)
        {
            for (auto m = 0; m < numM; ++m)
            {
                const double newPointXCoordinate = OriginXCoordinate + m * XGridBlockSize * cosineAngle - n * YGridBlockSize * sinAngle;
                double newPointYCoordinate = OriginYCoordinate + m * XGridBlockSize * sinAngle + n * YGridBlockSize * cosineAngle;
                if (m_projection == Projection::spherical && n > 0)
                {
                    newPointYCoordinate = XGridBlockSize * cos(degrad_hp * CurvilinearGrid.m_grid[n - 1][m].y);
                }
                CurvilinearGrid.m_grid[n][m] = {newPointXCoordinate, newPointYCoordinate};
            }
        }

        // in case a polygon is there, remove nodes outside
        if (!polygons.IsEmpty())
        {
            std::vector<std::vector<bool>> nodeBasedMask(numN, std::vector<bool>(numM, false));
            std::vector<std::vector<bool>> faceBasedMask(numN - 1, std::vector<bool>(numM - 1, false));
            // mark points inside a polygon
            for (auto n = 0; n < numN; ++n)
            {
                for (auto m = 0; m < numM; ++m)
                {
                    const bool isInPolygon = polygons.IsPointInPolygon(CurvilinearGrid.m_grid[n][m], 0);
                    if (isInPolygon)
                    {
                        nodeBasedMask[n][m] = true;
                    }
                }
            }

            // mark faces when at least one node is inside
            for (auto n = 0; n < numN - 1; ++n)
            {
                for (auto m = 0; m < numM - 1; ++m)
                {
                    if (nodeBasedMask[n][m] || nodeBasedMask[n + 1][m] || nodeBasedMask[n][m + 1] || nodeBasedMask[n + 1][m + 1])
                    {
                        faceBasedMask[n][m] = true;
                    }
                }
            }

            //mark nodes that are member of a cell inside the polygon(s)
            for (auto n = 0; n < numN - 1; ++n)
            {
                for (auto m = 0; m < numM - 1; ++m)
                {
                    if (faceBasedMask[n][m])
                    {
                        nodeBasedMask[n][m] = true;
                        nodeBasedMask[n + 1][m] = true;
                        nodeBasedMask[n][m + 1] = true;
                        nodeBasedMask[n + 1][m + 1] = true;
                    }
                }
            }

            // mark points inside a polygon
            for (auto n = 0; n < numN; ++n)
            {
                for (auto m = 0; m < numM; ++m)
                {
                    if (!nodeBasedMask[n][m])
                    {
                        CurvilinearGrid.m_grid[n][m].x = doubleMissingValue;
                        CurvilinearGrid.m_grid[n][m].y = doubleMissingValue;
                    }
                }
            }
        }
    }

    // Assign mesh
    *this = Mesh(CurvilinearGrid, m_projection);

    m_nodesRTreeRequiresUpdate = true;
    m_edgesRTreeRequiresUpdate = true;

    Administrate(AdministrationOptions::AdministrateMeshEdges);
}

void meshkernel::Mesh::MergeNodesInPolygon(const Polygons& polygon)
{
    // first filter the nodes in polygon
    std::vector<Point> filteredNodes(GetNumNodes());
    std::vector<size_t> originalNodeIndices(GetNumNodes(), sizetMissingValue);
    size_t index = 0;
    for (auto i = 0; i < GetNumNodes(); i++)
    {
        const bool inPolygon = polygon.IsPointInPolygon(m_nodes[i], 0);
        if (inPolygon)
        {
            filteredNodes[index] = m_nodes[i];
            originalNodeIndices[index] = i;
            index++;
        }
    }
    filteredNodes.resize(index);

    // Update the R-Tree of the mesh nodes
    SpatialTrees::RTree nodesRtree;
    nodesRtree.BuildTree(filteredNodes);

    // merge the closest nodes
    for (auto i = 0; i < filteredNodes.size(); i++)
    {
        nodesRtree.NearestNeighborsOnSquaredDistance(filteredNodes[i], mergingDistanceSquared);

        const auto resultSize = nodesRtree.GetQueryResultSize();
        if (resultSize > 1)
        {
            for (auto j = 0; j < nodesRtree.GetQueryResultSize(); j++)
            {
                const auto nodeIndexInFilteredNodes = nodesRtree.GetQuerySampleIndex(j);
                if (nodeIndexInFilteredNodes != i)
                {
                    MergeTwoNodes(originalNodeIndices[i], originalNodeIndices[nodeIndexInFilteredNodes]);
                    nodesRtree.DeleteNode(i);
                }
            }
        }
    }

    Administrate(AdministrationOptions::AdministrateMeshEdges);
}

void meshkernel::Mesh::MergeTwoNodes(size_t firstNodeIndex, size_t secondNodeIndex)
{
    if (firstNodeIndex >= GetNumNodes() || secondNodeIndex >= GetNumNodes())
    {
        throw std::invalid_argument("Mesh::MergeTwoNodes: Either the first or the second node-index is invalid.");
    }

    auto edgeIndex = FindEdge(firstNodeIndex, secondNodeIndex);
    if (edgeIndex != sizetMissingValue)
    {
        m_edges[edgeIndex].first = sizetMissingValue;
        m_edges[edgeIndex].second = sizetMissingValue;
    }

    // check if there is another edge starting at firstEdgeOtherNode and ending at secondNode
    for (auto n = 0; n < m_nodesNumEdges[firstNodeIndex]; n++)
    {
        const auto firstEdgeIndex = m_nodesEdges[firstNodeIndex][n];
        const auto firstEdge = m_edges[firstEdgeIndex];
        const auto firstEdgeOtherNode = OtherNodeOfEdge(firstEdge, firstNodeIndex);
        if (firstEdgeOtherNode != sizetMissingValue && firstEdgeOtherNode != secondNodeIndex)
        {
            for (auto nn = 0; nn < m_nodesNumEdges[firstEdgeOtherNode]; nn++)
            {
                const auto secondEdgeIndex = m_nodesEdges[firstEdgeOtherNode][nn];
                auto secondEdge = m_edges[secondEdgeIndex];
                const auto secondNodeSecondEdge = OtherNodeOfEdge(secondEdge, firstEdgeOtherNode);
                if (secondNodeSecondEdge == secondNodeIndex)
                {
                    m_edges[secondEdgeIndex].first = sizetMissingValue;
                    m_edges[secondEdgeIndex].second = sizetMissingValue;
                }
            }
        }
    }

    // add all valid edges starting at secondNode
    std::vector<size_t> secondNodeEdges(maximumNumberOfEdgesPerNode, sizetMissingValue);
    size_t numSecondNodeEdges = 0;
    for (auto n = 0; n < m_nodesNumEdges[secondNodeIndex]; n++)
    {
        edgeIndex = m_nodesEdges[secondNodeIndex][n];
        if (m_edges[edgeIndex].first != sizetMissingValue)
        {
            secondNodeEdges[numSecondNodeEdges] = edgeIndex;
            numSecondNodeEdges++;
        }
    }

    // add all valid edges starting at firstNode are assigned to the second node
    for (auto n = 0; n < m_nodesNumEdges[firstNodeIndex]; n++)
    {
        edgeIndex = m_nodesEdges[firstNodeIndex][n];
        if (m_edges[edgeIndex].first != sizetMissingValue)
        {
            secondNodeEdges[numSecondNodeEdges] = edgeIndex;
            if (m_edges[edgeIndex].first == firstNodeIndex)
            {
                m_edges[edgeIndex].first = secondNodeIndex;
            }
            if (m_edges[edgeIndex].second == firstNodeIndex)
            {
                m_edges[edgeIndex].second = secondNodeIndex;
            }
            numSecondNodeEdges++;
        }
    }

    // re-assign edges to second node
    m_nodesEdges[secondNodeIndex] = std::vector<size_t>(secondNodeEdges.begin(), secondNodeEdges.begin() + numSecondNodeEdges);
    m_nodesNumEdges[secondNodeIndex] = numSecondNodeEdges;

    // remove edges to first node
    m_nodesEdges[firstNodeIndex] = std::vector<size_t>(0);
    m_nodesNumEdges[firstNodeIndex] = 0;
    m_nodes[firstNodeIndex] = {doubleMissingValue, doubleMissingValue};

    m_nodesRTreeRequiresUpdate = true;
    m_edgesRTreeRequiresUpdate = true;
}

size_t meshkernel::Mesh::ConnectNodes(size_t startNode, size_t endNode)
{
    const auto edgeIndex = FindEdge(startNode, endNode);

    // The nodes are already connected
    if (edgeIndex != sizetMissingValue)
        return sizetMissingValue;

    // increment the edges container
    const auto newEdgeIndex = GetNumEdges();
    m_edges.resize(newEdgeIndex + 1);
    m_edges[newEdgeIndex].first = startNode;
    m_edges[newEdgeIndex].second = endNode;
    m_numEdges++;

    m_edgesRTreeRequiresUpdate = true;

    return newEdgeIndex;
}

size_t meshkernel::Mesh::InsertNode(const Point& newPoint)
{
    const auto newSize = GetNumNodes() + 1;
    const auto newNodeIndex = GetNumNodes();

    m_nodes.resize(newSize);
    m_nodeMask.resize(newSize);
    m_nodesNumEdges.resize(newSize);
    m_nodesEdges.resize(newSize);

    m_numNodes++;

    m_nodes[newNodeIndex] = newPoint;
    m_nodeMask[newNodeIndex] = static_cast<int>(newNodeIndex);
    m_nodesNumEdges[newNodeIndex] = 0;

    m_nodesRTreeRequiresUpdate = true;

    return newNodeIndex;
}

void meshkernel::Mesh::DeleteNode(size_t nodeIndex)
{
    if (nodeIndex >= GetNumNodes())
    {
        throw std::invalid_argument("Mesh::DeleteNode: The index of the node to be deleted does not exist.");
    }

    for (auto e = 0; e < m_nodesNumEdges[nodeIndex]; e++)
    {
        const auto edgeIndex = m_nodesEdges[nodeIndex][e];
        DeleteEdge(edgeIndex);
    }
    m_nodes[nodeIndex] = {doubleMissingValue, doubleMissingValue};
    m_numNodes--;

    m_nodesRTreeRequiresUpdate = true;
}

void meshkernel::Mesh::DeleteEdge(size_t edgeIndex)
{
    if (edgeIndex == sizetMissingValue)
    {
        throw std::invalid_argument("Mesh::DeleteEdge: The index of the edge to be deleted does not exist.");
    }

    m_edges[edgeIndex].first = sizetMissingValue;
    m_edges[edgeIndex].second = sizetMissingValue;

    m_edgesRTreeRequiresUpdate = true;
}

void meshkernel::Mesh::ComputeFaceClosedPolygonWithLocalMappings(size_t faceIndex,
                                                                 std::vector<Point>& polygonNodesCache,
                                                                 std::vector<size_t>& localNodeIndicesCache,
                                                                 std::vector<size_t>& globalEdgeIndicesCache) const
{
    const auto numFaceNodes = GetNumFaceEdges(faceIndex);
    polygonNodesCache.reserve(numFaceNodes + 1);
    polygonNodesCache.clear();
    localNodeIndicesCache.reserve(numFaceNodes + 1);
    localNodeIndicesCache.clear();
    globalEdgeIndicesCache.reserve(numFaceNodes + 1);
    globalEdgeIndicesCache.clear();

    for (auto n = 0; n < numFaceNodes; n++)
    {
        polygonNodesCache.emplace_back(m_nodes[m_facesNodes[faceIndex][n]]);
        localNodeIndicesCache.emplace_back(n);
        globalEdgeIndicesCache.emplace_back(m_facesEdges[faceIndex][n]);
    }
    polygonNodesCache.emplace_back(polygonNodesCache.front());
    localNodeIndicesCache.emplace_back(0);
    globalEdgeIndicesCache.emplace_back(globalEdgeIndicesCache.front());
}

void meshkernel::Mesh::ComputeFaceClosedPolygon(size_t faceIndex, std::vector<Point>& polygonNodesCache) const
{
    const auto numFaceNodes = GetNumFaceEdges(faceIndex);
    polygonNodesCache.clear();
    polygonNodesCache.reserve(numFaceNodes);
    for (auto n = 0; n < numFaceNodes; n++)
    {
        polygonNodesCache.push_back(m_nodes[m_facesNodes[faceIndex][n]]);
    }
    polygonNodesCache.push_back(polygonNodesCache.front());
}

void meshkernel::Mesh::MaskNodesInPolygons(const Polygons& polygon, bool inside)
{
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 0);
    for (auto i = 0; i < GetNumNodes(); ++i)
    {
        bool isInPolygon = polygon.IsPointInPolygons(m_nodes[i]);
        if (!inside)
        {
            isInPolygon = !isInPolygon;
        }
        m_nodeMask[i] = 0;
        if (isInPolygon)
        {
            m_nodeMask[i] = 1;
        }
    }
}

void meshkernel::Mesh::ComputeEdgesLengths()
{
    auto const numEdges = GetNumEdges();
    m_edgeLengths.resize(numEdges, doubleMissingValue);
    for (auto e = 0; e < numEdges; e++)
    {
        auto const first = m_edges[e].first;
        auto const second = m_edges[e].second;
        m_edgeLengths[e] = ComputeDistance(m_nodes[first], m_nodes[second], m_projection);
    }
}

void meshkernel::Mesh::ComputeEdgesCenters()
{
    m_edgesCenters = ComputeEdgeCenters(m_nodes, m_edges);
}

bool meshkernel::Mesh::IsFullFaceNotInPolygon(size_t faceIndex) const
{
    for (auto n = 0; n < GetNumFaceEdges(faceIndex); n++)
    {
        if (m_nodeMask[m_facesNodes[faceIndex][n]] != 1)
        {
            return true;
        }
    }
    return false;
}

size_t meshkernel::Mesh::FindCommonNode(size_t firstEdgeIndex, size_t secondEdgeIndex) const
{
    const auto firstEdgeFirstNode = m_edges[firstEdgeIndex].first;
    const auto firstEdgeEdgeSecondNode = m_edges[firstEdgeIndex].second;

    const auto secondEdgeFirstNode = m_edges[secondEdgeIndex].first;
    const auto secondEdgeSecondNode = m_edges[secondEdgeIndex].second;

    if (firstEdgeFirstNode == sizetMissingValue || firstEdgeEdgeSecondNode == sizetMissingValue || secondEdgeFirstNode == sizetMissingValue || secondEdgeSecondNode == sizetMissingValue)
    {
        throw std::invalid_argument("Mesh::FindCommonNode: At least one of the given edges is invalid.");
    }

    if (firstEdgeFirstNode == secondEdgeFirstNode || firstEdgeFirstNode == secondEdgeSecondNode)
    {
        return firstEdgeFirstNode;
    }
    if (firstEdgeEdgeSecondNode == secondEdgeFirstNode || firstEdgeEdgeSecondNode == secondEdgeSecondNode)
    {
        return firstEdgeEdgeSecondNode;
    }
    return sizetMissingValue;
}

size_t meshkernel::Mesh::FindEdge(size_t firstNodeIndex, size_t secondNodeIndex) const
{
    if (firstNodeIndex == sizetMissingValue || secondNodeIndex == sizetMissingValue)
    {
        throw std::invalid_argument("Mesh::FindEdge: Invalid node index.");
    }

    size_t edgeIndex = sizetMissingValue;
    for (auto n = 0; n < m_nodesNumEdges[firstNodeIndex]; n++)
    {
        const auto localEdgeIndex = m_nodesEdges[firstNodeIndex][n];
        const auto firstEdgeOtherNode = OtherNodeOfEdge(m_edges[localEdgeIndex], firstNodeIndex);
        if (firstEdgeOtherNode == secondNodeIndex)
        {
            edgeIndex = localEdgeIndex;
            break;
        }
    }
    return edgeIndex;
}

void meshkernel::Mesh::GetBoundingBox(Point& lowerLeft, Point& upperRight) const
{

    double minx = std::numeric_limits<double>::max();
    double maxx = std::numeric_limits<double>::lowest();
    double miny = std::numeric_limits<double>::max();
    double maxy = std::numeric_limits<double>::lowest();
    for (auto n = 0; n < GetNumNodes(); n++)
    {
        if (m_nodes[n].IsValid())
        {
            minx = std::min(minx, m_nodes[n].x);
            maxx = std::max(maxx, m_nodes[n].x);
            miny = std::min(miny, m_nodes[n].y);
            maxy = std::max(maxy, m_nodes[n].y);
        }
    }
    lowerLeft = {minx, miny};
    upperRight = {maxx, maxy};
}

void meshkernel::Mesh::OffsetSphericalCoordinates(double minx, double maxx)
{
    if (m_projection == Projection::spherical && maxx - minx > 180.0)
    {
        for (auto n = 0; n < GetNumNodes(); ++n)
        {
            if (m_nodes[n].x - 360.0 >= minx)
            {
                m_nodes[n].x -= 360.0;
            }

            if (m_nodes[n].x < minx)
            {
                m_nodes[n].x += 360.0;
            }
        }
    }
}

size_t meshkernel::Mesh::GetNodeIndex(Point point, double searchRadius)
{
    if (GetNumNodes() <= 0)
    {
        throw std::invalid_argument("Mesh::GetNodeIndex: There are no valid nodes.");
    }

    // create rtree a first time
    if (m_nodesRTree.Empty())
    {
        m_nodesRTree.BuildTree(m_nodes);
        m_nodesRTreeRequiresUpdate = false;
    }

    double const searchRadiusSquared = searchRadius * searchRadius;
    m_nodesRTree.NearestNeighborsOnSquaredDistance(point, searchRadiusSquared);
    const auto resultSize = m_nodesRTree.GetQueryResultSize();

    if (resultSize > 0)
    {
        return m_nodesRTree.GetQuerySampleIndex(0);
    }

    throw AlgorithmError("Mesh::GetNodeIndex: Could not find the node index close to a point.");
}

size_t meshkernel::Mesh::FindEdgeCloseToAPoint(Point point)
{
    if (GetNumEdges() == 0)
    {
        throw std::invalid_argument("Mesh::GetNodeIndex: There are no valid edges.");
    }

    if (m_edgesRTree.Empty())
    {
        ComputeEdgesCenters();
        m_edgesRTree.BuildTree(m_edgesCenters);
        m_edgesRTreeRequiresUpdate = false;
    }

    m_edgesRTree.NearestNeighbors(point);
    auto const resultSize = m_edgesRTree.GetQueryResultSize();
    if (resultSize >= 1)
    {
        const auto edgeIndex = m_edgesRTree.GetQuerySampleIndex(0);
        return edgeIndex;
    }

    throw AlgorithmError("Mesh::FindEdgeCloseToAPoint: Could not find the closest edge to a point.");
}

void meshkernel::Mesh::MaskFaceEdgesInPolygon(const Polygons& polygons, bool invertSelection, bool includeIntersected)
{
    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);

    // mark all nodes in polygon with 1
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 0);
    for (auto n = 0; n < GetNumNodes(); ++n)
    {
        const auto isInPolygon = polygons.IsPointInPolygon(m_nodes[n], 0);
        if (isInPolygon)
        {
            m_nodeMask[n] = 1;
        }
    }

    // mark all edges with both start end end nodes included with 1
    std::vector<int> edgeMask(m_edges.size(), 0);
    for (auto e = 0; e < GetNumEdges(); ++e)
    {
        const auto firstNodeIndex = m_edges[e].first;
        const auto secondNodeIndex = m_edges[e].second;

        int isEdgeIncluded;
        if (includeIntersected)
        {
            isEdgeIncluded = (firstNodeIndex != sizetMissingValue && m_nodeMask[firstNodeIndex] == 1 ||
                              secondNodeIndex != sizetMissingValue && m_nodeMask[secondNodeIndex] == 1)
                                 ? 1
                                 : 0;
        }
        else
        {
            isEdgeIncluded = (firstNodeIndex != sizetMissingValue && m_nodeMask[firstNodeIndex] == 1 &&
                              secondNodeIndex != sizetMissingValue && m_nodeMask[secondNodeIndex] == 1)
                                 ? 1
                                 : 0;
        }

        edgeMask[e] = isEdgeIncluded;
    }

    // if one edge of the face is not included do not include all the edges of that face
    auto secondEdgeMask = edgeMask;
    if (!includeIntersected)
    {
        for (auto f = 0; f < GetNumFaces(); ++f)
        {
            bool isOneEdgeNotIncluded = false;
            for (auto n = 0; n < GetNumFaceEdges(f); ++n)
            {
                const auto edgeIndex = m_facesEdges[f][n];
                if (edgeIndex != sizetMissingValue && edgeMask[edgeIndex] == 0)
                {
                    isOneEdgeNotIncluded = true;
                    break;
                }
            }

            if (isOneEdgeNotIncluded)
            {
                for (auto n = 0; n < GetNumFaceEdges(f); ++n)
                {
                    const auto edgeIndex = m_facesEdges[f][n];
                    if (edgeIndex != sizetMissingValue)
                    {
                        secondEdgeMask[edgeIndex] = 0;
                    }
                }
            }
        }
    }

    // if the selection is inverted, do not delete the edges of included faces
    if (invertSelection)
    {
        for (auto e = 0; e < GetNumEdges(); ++e)
        {
            if (secondEdgeMask[e] == 0)
            {
                secondEdgeMask[e] = 1;
            }

            if (edgeMask[e] == 1)
            {
                secondEdgeMask[e] = 0;
            }
        }
    }

    m_edgeMask = std::move(secondEdgeMask);
}

void meshkernel::Mesh::DeleteMesh(const Polygons& polygons, int deletionOption, bool invertDeletion)
{
    if (deletionOption == AllNodesInside)
    {
        for (auto n = 0; n < GetNumNodes(); ++n)
        {
            auto isInPolygon = polygons.IsPointInPolygon(m_nodes[n], 0);
            if (invertDeletion)
            {
                isInPolygon = !isInPolygon;
            }
            if (isInPolygon)
            {
                m_nodes[n] = {doubleMissingValue, doubleMissingValue};
            }
        }
    }

    if (deletionOption == FacesWithIncludedCircumcenters)
    {
        Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);

        for (auto e = 0; e < GetNumEdges(); ++e)
        {
            bool allFaceCircumcentersInPolygon = true;

            for (auto f = 0; f < GetNumEdgesFaces(e); ++f)
            {
                const auto faceIndex = m_edgesFaces[e][f];
                if (faceIndex == sizetMissingValue)
                {
                    continue;
                }

                const auto faceCircumcenter = m_facesCircumcenters[faceIndex];
                auto isInPolygon = polygons.IsPointInPolygon(faceCircumcenter, 0);
                if (invertDeletion)
                {
                    isInPolygon = !isInPolygon;
                }
                if (!isInPolygon)
                {
                    allFaceCircumcentersInPolygon = false;
                    break;
                }
            }

            // 2D edge without surrounding faces.
            if (GetNumEdgesFaces(e) == 0)
            {
                const auto firstNodeIndex = m_edges[e].first;
                const auto secondNodeIndex = m_edges[e].second;

                if (firstNodeIndex == sizetMissingValue || secondNodeIndex == sizetMissingValue)
                {
                    continue;
                }

                const auto edgeCenter = (m_nodes[firstNodeIndex] + m_nodes[secondNodeIndex]) / 2.0;

                allFaceCircumcentersInPolygon = polygons.IsPointInPolygon(edgeCenter, 0);
                if (invertDeletion)
                {
                    allFaceCircumcentersInPolygon = !allFaceCircumcentersInPolygon;
                }
            }

            if (allFaceCircumcentersInPolygon)
            {
                m_edges[e].first = sizetMissingValue;
                m_edges[e].second = sizetMissingValue;
            }
        }
    }

    if (deletionOption == FacesCompletelyIncluded)
    {
        MaskFaceEdgesInPolygon(polygons, invertDeletion, false);

        // mark the edges for deletion
        for (auto e = 0; e < GetNumEdges(); ++e)
        {
            if (m_edgeMask[e] == 1)
            {
                m_edges[e].first = sizetMissingValue;
                m_edges[e].second = sizetMissingValue;
            }
        }
    }

    m_nodesRTreeRequiresUpdate = true;
    m_edgesRTreeRequiresUpdate = true;

    Administrate(AdministrationOptions::AdministrateMeshEdges);
}

void meshkernel::Mesh::MoveNode(Point newPoint, size_t nodeindex)
{
    const Point nodeToMove = m_nodes[nodeindex];

    const auto dx = GetDx(nodeToMove, newPoint, m_projection);
    const auto dy = GetDy(nodeToMove, newPoint, m_projection);

    const auto distanceNodeToMoveFromNewPoint = std::sqrt(dx * dx + dy * dy);
    for (auto n = 0; n < GetNumNodes(); ++n)
    {
        const auto nodeDx = GetDx(m_nodes[n], nodeToMove, m_projection);
        const auto nodeDy = GetDy(m_nodes[n], nodeToMove, m_projection);
        const double distanceCurrentNodeFromNewPoint = std::sqrt(nodeDx * nodeDx + nodeDy * nodeDy);

        const auto factor = 0.5 * (1.0 + std::cos(std::min(distanceCurrentNodeFromNewPoint / distanceNodeToMoveFromNewPoint, 1.0) * M_PI));

        m_nodes[n].x += dx * factor;
        m_nodes[n].y += dy * factor;
    }

    m_nodesRTreeRequiresUpdate = true;
    m_edgesRTreeRequiresUpdate = true;
}

meshkernel::Mesh& meshkernel::Mesh::operator+=(Mesh const& rhs)
{
    if (m_projection != rhs.m_projection || rhs.GetNumNodes() == 0 || rhs.GetNumEdges() == 0)
    {
        throw std::invalid_argument("Mesh::operator+=: The two meshes cannot be added.");
    }

    const auto rhsNumNodes = rhs.GetNumNodes();
    const auto rhsNumEdges = rhs.GetNumEdges();

    m_edges.resize(GetNumEdges() + rhsNumEdges);
    m_nodes.resize(GetNumNodes() + rhsNumNodes);

    //copy mesh nodes
    for (auto n = GetNumNodes(); n < GetNumNodes() + rhsNumNodes; ++n)
    {
        const auto index = n - GetNumNodes();
        m_nodes[n] = rhs.m_nodes[index];
    }

    //copy mesh edges
    for (auto e = GetNumEdges(); e < GetNumEdges() + rhsNumEdges; ++e)
    {
        const auto index = e - GetNumEdges();
        m_edges[e].first = rhs.m_edges[index].first + GetNumNodes();
        m_edges[e].second = rhs.m_edges[index].second + GetNumNodes();
    }

    m_nodesRTreeRequiresUpdate = true;
    m_edgesRTreeRequiresUpdate = true;

    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);

    //no polygon involved, so node mask is 1 everywhere
    m_nodeMask.resize(m_nodes.size());
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 1);

    return *this;
}

void meshkernel::Mesh::ComputeNodeMaskFromEdgeMask()
{
    if (m_edgeMask.size() != GetNumEdges() || m_nodeMask.size() != GetNumNodes())
    {
        throw std::invalid_argument("Mesh::ComputeNodeMaskFromEdgeMask:The dimension of the masks do not fit the mesh.");
    }

    // fill node mask to 0
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 0);

    // compute node mask from edge mask
    for (auto e = 0; e < GetNumEdges(); ++e)
    {
        if (m_edgeMask[e] != 1)
            continue;

        const auto firstNodeIndex = m_edges[e].first;
        const auto secondNodeIndex = m_edges[e].second;

        if (firstNodeIndex != sizetMissingValue)
        {
            m_nodeMask[firstNodeIndex] = 1;
        }
        if (secondNodeIndex != sizetMissingValue)
        {
            m_nodeMask[secondNodeIndex] = 1;
        }
    }
}

bool meshkernel::Mesh::IsFaceOnBoundary(size_t face) const
{
    bool isFaceOnBoundary = false;

    for (auto e = 0; e < GetNumFaceEdges(face); ++e)
    {
        const auto edge = m_facesEdges[face][e];
        if (IsEdgeOnBoundary(edge))
        {
            isFaceOnBoundary = true;
            break;
        }
    }
    return isFaceOnBoundary;
}

meshkernel::Point meshkernel::Mesh::ComputeFaceCircumenter(std::vector<Point>& polygon,
                                                           const std::vector<size_t>& edgesNumFaces) const
{
    const size_t maximumNumberCircumcenterIterations = 100;
    const double eps = m_projection == Projection::cartesian ? 1e-3 : 9e-10; //111km = 0-e digit.
    std::vector<Point> middlePoints;
    middlePoints.reserve(maximumNumberOfNodesPerFace);
    std::vector<Point> normals;
    normals.reserve(maximumNumberOfNodesPerFace);
    const auto numNodes = polygon.size() - 1;

    Point centerOfMass{0.0, 0.0};
    for (auto n = 0; n < numNodes; n++)
    {
        centerOfMass.x += polygon[n].x;
        centerOfMass.y += polygon[n].y;
    }
    centerOfMass = centerOfMass / numNodes;

    auto result = centerOfMass;
    if (numNodes == numNodesInTriangle)
    {
        result = CircumcenterOfTriangle(polygon[0], polygon[1], polygon[2], m_projection);
    }
    else if (!edgesNumFaces.empty())
    {
        size_t numValidEdges = 0;
        for (auto n = 0; n < numNodes; ++n)
        {
            if (edgesNumFaces[n] == 2)
            {
                numValidEdges++;
            }
        }

        if (numValidEdges > 1)
        {
            for (auto n = 0; n < numNodes; n++)
            {
                if (edgesNumFaces[n] != 2)
                {
                    continue;
                }
                const auto nextNode = NextCircularForwardIndex(n, numNodes);
                middlePoints.emplace_back((polygon[n] + polygon[nextNode]) * 0.5);
                normals.emplace_back(NormalVector(polygon[n], polygon[nextNode], middlePoints.back(), m_projection));
            }

            Point estimatedCircumCenter = centerOfMass;
            for (auto iter = 0; iter < maximumNumberCircumcenterIterations; ++iter)
            {
                const Point previousCircumCenter = estimatedCircumCenter;
                for (auto n = 0; n < middlePoints.size(); n++)
                {
                    const auto dx = GetDx(middlePoints[n], estimatedCircumCenter, m_projection);
                    const auto dy = GetDy(middlePoints[n], estimatedCircumCenter, m_projection);
                    const auto increment = -0.1 * DotProduct(dx, normals[n].x, dy, normals[n].y);
                    AddIncrementToPoint(normals[n], increment, centerOfMass, m_projection, estimatedCircumCenter);
                }
                if (iter > 0 &&
                    abs(estimatedCircumCenter.x - previousCircumCenter.x) < eps &&
                    abs(estimatedCircumCenter.y - previousCircumCenter.y) < eps)
                {
                    result = estimatedCircumCenter;
                    break;
                }
            }
        }
    }

    for (auto n = 0; n < numNodes; n++)
    {
        polygon[n].x = weightCircumCenter * polygon[n].x + (1.0 - weightCircumCenter) * centerOfMass.x;
        polygon[n].y = weightCircumCenter * polygon[n].y + (1.0 - weightCircumCenter) * centerOfMass.y;
    }

    if (IsPointInPolygonNodes(result, polygon, m_projection))
    {
        return result;
    }

    for (auto n = 0; n < numNodes; n++)
    {
        const auto nextNode = NextCircularForwardIndex(n, numNodes);
        Point intersection;
        double crossProduct;
        double firstRatio;
        double secondRatio;
        const auto areLineCrossing = AreSegmentsCrossing(centerOfMass, result, polygon[n], polygon[nextNode], false, m_projection, intersection, crossProduct, firstRatio, secondRatio);
        if (areLineCrossing)
        {
            result = intersection;
            break;
        }
    }

    return result;
}

std::vector<meshkernel::Point> meshkernel::Mesh::GetObtuseTrianglesCenters()
{
    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);
    std::vector<Point> result;
    result.reserve(GetNumFaces());
    for (auto f = 0; f < GetNumFaces(); ++f)
    {
        // a triangle
        if (m_numFacesNodes[f] == 3)
        {
            const auto firstNode = m_facesNodes[f][0];
            const auto secondNode = m_facesNodes[f][1];
            const auto thirdNode = m_facesNodes[f][2];
            //compute squared edge lengths
            const auto firstEdgeSquaredLength = ComputeSquaredDistance(m_nodes[secondNode], m_nodes[firstNode], m_projection);
            const auto secondEdgeSquaredLength = ComputeSquaredDistance(m_nodes[thirdNode], m_nodes[firstNode], m_projection);
            const auto thirdEdgeSquaredLength = ComputeSquaredDistance(m_nodes[thirdNode], m_nodes[secondNode], m_projection);

            if (firstEdgeSquaredLength > secondEdgeSquaredLength + thirdEdgeSquaredLength ||
                secondEdgeSquaredLength > firstEdgeSquaredLength + thirdEdgeSquaredLength ||
                thirdEdgeSquaredLength > secondEdgeSquaredLength + firstEdgeSquaredLength)
            {
                result.emplace_back(m_facesMassCenters[f]);
            }
        }
    }
    return result;
}

std::vector<size_t> meshkernel::Mesh::GetEdgesCrossingSmallFlowEdges(double smallFlowEdgesThreshold)
{
    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);
    std::vector<size_t> result;
    result.reserve(GetNumEdges());
    for (auto e = 0; e < GetNumEdges(); ++e)
    {
        const auto firstFace = m_edgesFaces[e][0];
        const auto secondFace = m_edgesFaces[e][1];

        if (firstFace != sizetMissingValue && secondFace != sizetMissingValue)
        {
            const auto flowEdgeLength = ComputeDistance(m_facesCircumcenters[firstFace], m_facesCircumcenters[secondFace], m_projection);
            const double cutOffDistance = smallFlowEdgesThreshold * 0.5 * (std::sqrt(m_faceArea[firstFace]) + std::sqrt(m_faceArea[secondFace]));

            if (flowEdgeLength < cutOffDistance)
            {
                result.emplace_back(e);
            }
        }
    }
    return result;
}

std::vector<meshkernel::Point> meshkernel::Mesh::GetFlowEdgesCenters(const std::vector<size_t>& edges) const
{
    std::vector<Point> result;
    result.reserve(GetNumEdges());
    for (const auto& edge : edges)
    {
        const auto firstFace = m_edgesFaces[edge][0];
        const auto secondFace = m_edgesFaces[edge][1];
        result.emplace_back((m_facesCircumcenters[firstFace] + m_facesCircumcenters[secondFace]) * 0.5);
    }

    return result;
}

void meshkernel::Mesh::DeleteSmallFlowEdges(double smallFlowEdgesThreshold)
{
    DeleteDegeneratedTriangles();

    auto edges = GetEdgesCrossingSmallFlowEdges(smallFlowEdgesThreshold);
    if (!edges.empty())
    {
        // invalidate the edges
        for (const auto& e : edges)
        {
            m_edges[e] = {sizetMissingValue, sizetMissingValue};
        }
        Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);
    }
}

void meshkernel::Mesh::DeleteSmallTrianglesAtBoundaries(double minFractionalAreaTriangles)
{
    // On the second part, the small triangles at the boundaries are checked
    const double minCosPhi = 0.2;
    std::vector<std::vector<size_t>> smallTrianglesNodes;
    for (auto face = 0; face < GetNumFaces(); ++face)
    {
        if (m_numFacesNodes[face] != numNodesInTriangle || m_faceArea[face] <= 0.0 || !IsFaceOnBoundary(face))
        {
            continue;
        }

        // compute the average area of neighboring faces
        double averageOtherFacesArea = 0.0;
        size_t numNonBoundaryFaces = 0;
        for (auto e = 0; e < numNodesInTriangle; ++e)
        {
            // the edge must not be at the boundary, otherwise there is no "other" face
            const auto edge = m_facesEdges[face][e];
            if (IsEdgeOnBoundary(edge))
            {
                continue;
            }
            const auto otherFace = face == m_edgesFaces[edge][0] ? m_edgesFaces[edge][1] : m_edgesFaces[edge][0];
            if (m_numFacesNodes[otherFace] > numNodesInTriangle)
            {
                averageOtherFacesArea += m_faceArea[otherFace];
                numNonBoundaryFaces++;
            }
        }

        if (numNonBoundaryFaces == 0 || m_faceArea[face] / (averageOtherFacesArea / double(numNonBoundaryFaces)) > minFractionalAreaTriangles)
        {
            // no valid boundary faces, the area of the current triangle is larger enough compared to the neighbors
            continue;
        }

        double minCosPhiSmallTriangle = 1.0;
        size_t nodeToPreserve = sizetMissingValue;
        size_t firstNodeToMerge;
        size_t secondNodeToMerge;
        size_t thirdEdgeSmallTriangle = sizetMissingValue;
        for (auto e = 0; e < numNodesInTriangle; ++e)
        {
            const auto previousEdge = NextCircularBackwardIndex(e, numNodesInTriangle);
            const auto nextEdge = NextCircularForwardIndex(e, numNodesInTriangle);

            const auto k0 = m_facesNodes[face][previousEdge];
            const auto k1 = m_facesNodes[face][e];
            const auto k2 = m_facesNodes[face][nextEdge];

            // compute the angles between the edges
            const auto cosphi = std::abs(NormalizedInnerProductTwoSegments(m_nodes[k0], m_nodes[k1], m_nodes[k1], m_nodes[k2], m_projection));

            if (cosphi < minCosPhiSmallTriangle)
            {
                minCosPhiSmallTriangle = cosphi;
                firstNodeToMerge = k0;
                nodeToPreserve = k1;
                secondNodeToMerge = k2;
                thirdEdgeSmallTriangle = m_facesEdges[face][nextEdge];
            }
        }

        if (minCosPhiSmallTriangle < minCosPhi && thirdEdgeSmallTriangle != sizetMissingValue && IsEdgeOnBoundary(thirdEdgeSmallTriangle))
        {
            smallTrianglesNodes.emplace_back(std::initializer_list<size_t>{nodeToPreserve, firstNodeToMerge, secondNodeToMerge});
        }
    }

    bool nodesMerged = false;
    for (const auto& triangleNodes : smallTrianglesNodes)
    {
        const auto nodeToPreserve = triangleNodes[0];
        const auto firstNodeToMerge = triangleNodes[1];
        const auto secondNodeToMerge = triangleNodes[2];

        // only
        size_t numInternalEdges = 0;
        for (auto e = 0; e < m_nodesNumEdges[firstNodeToMerge]; ++e)
        {
            if (!IsEdgeOnBoundary(m_nodesEdges[firstNodeToMerge][e]))
            {
                numInternalEdges++;
            }
        }

        if (numInternalEdges == 1)
        {
            MergeTwoNodes(firstNodeToMerge, nodeToPreserve);
            nodesMerged = true;
        }

        // corner point of a triangle
        numInternalEdges = 0;
        for (auto e = 0; e < m_nodesNumEdges[secondNodeToMerge]; ++e)
        {
            if (!IsEdgeOnBoundary(m_nodesEdges[secondNodeToMerge][e]))
            {
                numInternalEdges++;
            }
        }

        if (numInternalEdges == 1)
        {
            MergeTwoNodes(secondNodeToMerge, nodeToPreserve);
            nodesMerged = true;
        }
    }

    if (nodesMerged)
    {
        Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);
    }
}

void meshkernel::Mesh::ComputeNodeNeighbours()
{
    m_maxNumNeighbours = *(std::max_element(m_nodesNumEdges.begin(), m_nodesNumEdges.end()));
    m_maxNumNeighbours += 1;

    m_nodesNodes.resize(GetNumNodes(), std::vector<size_t>(m_maxNumNeighbours, sizetMissingValue));
    //for each node, determine the neighbouring nodes
    for (auto n = 0; n < GetNumNodes(); n++)
    {
        for (auto nn = 0; nn < m_nodesNumEdges[n]; nn++)
        {
            const auto edge = m_edges[m_nodesEdges[n][nn]];
            m_nodesNodes[n][nn] = OtherNodeOfEdge(edge, n);
        }
    }
}

std::vector<double> meshkernel::Mesh::GetOrthogonality()
{
    std::vector<double> result;
    result.reserve(GetNumEdges());
    for (auto e = 0; e < GetNumEdges(); e++)
    {
        auto val = doubleMissingValue;
        const auto firstNode = m_edges[e].first;
        const auto secondNode = m_edges[e].second;

        if (firstNode != sizetMissingValue && secondNode != sizetMissingValue && !IsEdgeOnBoundary(e))
        {
            val = NormalizedInnerProductTwoSegments(m_nodes[firstNode],
                                                    m_nodes[secondNode],
                                                    m_facesCircumcenters[m_edgesFaces[e][0]],
                                                    m_facesCircumcenters[m_edgesFaces[e][1]],
                                                    m_projection);
            if (val != doubleMissingValue)
            {
                val = std::abs(val);
            }
        }
        result.emplace_back(val);
    }
    return result;
}

std::vector<double> meshkernel::Mesh::GetSmoothness()
{
    std::vector<double> result;
    result.reserve(GetNumEdges());
    for (auto e = 0; e < GetNumEdges(); e++)
    {
        auto val = doubleMissingValue;
        const auto firstNode = m_edges[e].first;
        const auto secondNode = m_edges[e].second;

        if (firstNode != sizetMissingValue && secondNode != sizetMissingValue && !IsEdgeOnBoundary(e))
        {
            const auto leftFace = m_edgesFaces[e][0];
            const auto rightFace = m_edgesFaces[e][1];
            const auto leftFaceArea = m_faceArea[leftFace];
            const auto rightFaceArea = m_faceArea[rightFace];

            if (leftFaceArea < minimumCellArea || rightFaceArea < minimumCellArea)
            {
                val = rightFaceArea / leftFaceArea;
            }
            if (val < 1.0)
            {
                val = 1.0 / val;
            }
        }
        result.emplace_back(val);
    }
    return result;
}

void meshkernel::Mesh::ComputeAspectRatios(std::vector<double>& aspectRatios)
{
    std::vector<std::vector<double>> averageEdgesLength(GetNumEdges(), std::vector<double>(2, doubleMissingValue));
    std::vector<double> averageFlowEdgesLength(GetNumEdges(), doubleMissingValue);
    std::vector<bool> curvilinearGridIndicator(GetNumNodes(), true);
    std::vector<double> edgesLength(GetNumEdges(), 0.0);
    aspectRatios.resize(GetNumEdges(), 0.0);

    for (auto e = 0; e < GetNumEdges(); e++)
    {
        const auto first = m_edges[e].first;
        const auto second = m_edges[e].second;

        if (first == second)
            continue;
        const double edgeLength = ComputeDistance(m_nodes[first], m_nodes[second], m_projection);
        edgesLength[e] = edgeLength;

        Point leftCenter;
        Point rightCenter;
        if (m_edgesNumFaces[e] > 0)
        {
            leftCenter = m_facesCircumcenters[m_edgesFaces[e][0]];
        }
        else
        {
            leftCenter = m_nodes[first];
        }

        //find right cell center, if it exists
        if (m_edgesNumFaces[e] == 2)
        {
            rightCenter = m_facesCircumcenters[m_edgesFaces[e][1]];
        }
        else
        {
            //otherwise, make ghost node by imposing boundary condition
            double dinry = InnerProductTwoSegments(m_nodes[first], m_nodes[second], m_nodes[first], leftCenter, m_projection);
            dinry = dinry / std::max(edgeLength * edgeLength, minimumEdgeLength);

            const double x0_bc = (1.0 - dinry) * m_nodes[first].x + dinry * m_nodes[second].x;
            const double y0_bc = (1.0 - dinry) * m_nodes[first].y + dinry * m_nodes[second].y;
            rightCenter.x = 2.0 * x0_bc - leftCenter.x;
            rightCenter.y = 2.0 * y0_bc - leftCenter.y;
        }

        averageFlowEdgesLength[e] = ComputeDistance(leftCenter, rightCenter, m_projection);
    }

    // Compute normal length
    for (auto f = 0; f < GetNumFaces(); f++)
    {
        const auto numberOfFaceNodes = GetNumFaceEdges(f);
        if (numberOfFaceNodes < numNodesInTriangle)
            continue;

        for (auto n = 0; n < numberOfFaceNodes; n++)
        {
            if (numberOfFaceNodes != numNodesQuads)
                curvilinearGridIndicator[m_facesNodes[f][n]] = false;
            const auto edgeIndex = m_facesEdges[f][n];

            if (m_edgesNumFaces[edgeIndex] == 0)
            {
                continue;
            }

            double edgeLength = edgesLength[edgeIndex];
            if (edgeLength != 0.0)
            {
                aspectRatios[edgeIndex] = averageFlowEdgesLength[edgeIndex] / edgeLength;
            }

            //quads
            if (numberOfFaceNodes == numNodesQuads)
            {
                size_t kkp2 = n + 2;
                if (kkp2 >= numberOfFaceNodes)
                {
                    kkp2 = kkp2 - numberOfFaceNodes;
                }

                const auto klinkp2 = m_facesEdges[f][kkp2];
                edgeLength = 0.5 * (edgesLength[edgeIndex] + edgesLength[klinkp2]);
            }

            if (IsEqual(averageEdgesLength[edgeIndex][0], doubleMissingValue))
            {
                averageEdgesLength[edgeIndex][0] = edgeLength;
            }
            else
            {
                averageEdgesLength[edgeIndex][1] = edgeLength;
            }
        }
    }

    if (curvilinearToOrthogonalRatio == 1.0)
        return;

    for (auto e = 0; e < GetNumEdges(); e++)
    {
        const auto first = m_edges[e].first;
        const auto second = m_edges[e].second;

        if (first == sizetMissingValue || second == sizetMissingValue)
            continue;
        if (m_edgesNumFaces[e] == 0)
            continue;
        // Consider only quads
        if (!curvilinearGridIndicator[first] || !curvilinearGridIndicator[second])
            continue;

        if (IsEdgeOnBoundary(e))
        {
            if (averageEdgesLength[e][0] > 0.0 &&
                IsEqual(averageEdgesLength[e][0], doubleMissingValue))
            {
                aspectRatios[e] = averageFlowEdgesLength[e] / averageEdgesLength[e][0];
            }
        }
        else
        {
            if (averageEdgesLength[e][0] > 0.0 &&
                averageEdgesLength[e][1] > 0.0 &&
                IsEqual(averageEdgesLength[e][0], doubleMissingValue) &&
                IsEqual(averageEdgesLength[e][1], doubleMissingValue))
            {
                aspectRatios[e] = curvilinearToOrthogonalRatio * aspectRatios[e] +
                                  (1.0 - curvilinearToOrthogonalRatio) * averageFlowEdgesLength[e] / (0.5 * (averageEdgesLength[e][0] + averageEdgesLength[e][1]));
            }
        }
    }
}

void meshkernel::Mesh::TriangulateFaces()
{
    for (auto i = 0; i < GetNumFaces(); i++)
    {
        const auto NumEdges = GetNumFaceEdges(i);

        if (NumEdges < 4)
        {
            continue;
        }

        const auto indexFirstNode = m_facesNodes[i][0];
        for (auto j = 2; j < NumEdges - 1; j++)
        {
            const auto nodeIndex = m_facesNodes[i][j];
            ConnectNodes(indexFirstNode, nodeIndex);
        }
    }

    m_edgesRTreeRequiresUpdate = true;
}

void meshkernel::Mesh::MakeDualFace(size_t node, double enlargementFactor, std::vector<Point>& dualFace)
{
    const auto sortedFacesIndices = SortedFacesAroundNode(node);
    const auto numEdges = m_nodesNumEdges[node];
    dualFace.reserve(maximumNumberOfEdgesPerNode);
    dualFace.clear();

    for (auto e = 0; e < numEdges; ++e)
    {
        const auto edgeIndex = m_nodesEdges[node][e];
        auto edgeCenter = m_edgesCenters[edgeIndex];

        if (m_projection == Projection::spherical)
        {
            const auto firstNodeIndex = m_edges[edgeIndex].first;
            const auto secondNodeIndex = m_edges[edgeIndex].second;

            if (firstNodeIndex != sizetMissingValue && secondNodeIndex != sizetMissingValue)
            {
                const auto diff = m_nodes[firstNodeIndex].x - m_nodes[secondNodeIndex].x;

                if (diff > 180.0)
                {
                    edgeCenter.x = edgeCenter.x - 180.0;
                }
                if (diff < -180.0)
                {
                    edgeCenter.x = edgeCenter.x + 180.0;
                }
            }
        }
        dualFace.emplace_back(edgeCenter);

        const auto faceIndex = sortedFacesIndices[e];
        if (faceIndex != sizetMissingValue)
        {
            dualFace.emplace_back(m_facesMassCenters[faceIndex]);
        }
        else
        {
            dualFace.emplace_back(m_nodes[node]);
        }
    }
    dualFace.emplace_back(dualFace[0]);

    // now we can compute the mass center of the dual face
    double area;
    Point centerOfMass;
    bool isCounterClockWise;
    FaceAreaAndCenterOfMass(dualFace, m_projection, area, centerOfMass, isCounterClockWise);

    if (m_projection == Projection::spherical)
    {
        if (centerOfMass.x - m_nodes[node].x > 180.0)
        {
            centerOfMass.x -= 360.0;
        }
        if (centerOfMass.x - m_nodes[node].x < 180.0)
        {
            centerOfMass.x += 360.0;
        }
    }

    for (auto& v : dualFace)
    {
        v = centerOfMass + (v - centerOfMass) * enlargementFactor;
    }
}

std::vector<size_t> meshkernel::Mesh::SortedFacesAroundNode(size_t node) const
{

    const auto numEdges = m_nodesNumEdges[node];
    std::vector<size_t> result;
    for (auto e = 0; e < numEdges; ++e)
    {
        const auto firstEdge = m_nodesEdges[node][e];

        // no faces for this edge
        if (m_edgesNumFaces[firstEdge] == 0)
        {
            continue;
        }

        auto const ee = NextCircularForwardIndex(e, numEdges);
        const auto secondEdge = m_nodesEdges[node][ee];
        const auto firstFace = m_edgesFaces[firstEdge][0];

        size_t secondFace = sizetMissingValue;
        if (m_edgesNumFaces[firstEdge] > 1)
        {
            secondFace = m_edgesFaces[firstEdge][1];
        }

        // check if the first face contains the first edge
        size_t firstEdgeIndexInFirstFace = 0;
        for (auto n = 0; n < m_numFacesNodes[firstFace]; ++n)
        {
            if (m_facesEdges[firstFace][n] == firstEdge)
            {
                firstEdgeIndexInFirstFace = n;
                break;
            }
        }

        // check if previous edge in firstFace is secondEdge (so at least two edges share the same edge)
        auto const secondEdgeindexInFirstFace = NextCircularBackwardIndex(firstEdgeIndexInFirstFace, m_numFacesNodes[firstFace]);

        if (m_facesEdges[firstFace][secondEdgeindexInFirstFace] == secondEdge)
        {
            result.emplace_back(firstFace);
        }
        else
        {
            result.emplace_back(secondFace);
        }
    }

    return result;
}

std::vector<meshkernel::Point> meshkernel::Mesh::MeshBoundaryToPolygon(const std::vector<Point>& polygonNodes)
{

    // Find faces
    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);
    std::vector<bool> isVisited(GetNumEdges(), false);
    std::vector<Point> meshBoundaryPolygon;
    meshBoundaryPolygon.reserve(GetNumNodes());

    for (auto e = 0; e < GetNumEdges(); e++)
    {
        if (isVisited[e] || !IsEdgeOnBoundary(e))
        {
            continue;
        }

        const auto firstNodeIndex = m_edges[e].first;
        const auto secondNodeIndex = m_edges[e].second;
        const auto firstNode = m_nodes[firstNodeIndex];
        const auto secondNode = m_nodes[secondNodeIndex];

        const auto firstNodeInPolygon = IsPointInPolygonNodes(m_nodes[firstNodeIndex], polygonNodes, m_projection);
        const auto secondNodeInPolygon = IsPointInPolygonNodes(m_nodes[secondNodeIndex], polygonNodes, m_projection);

        if (!firstNodeInPolygon && !secondNodeInPolygon)
        {
            continue;
        }

        //Start a new polyline
        if (!meshBoundaryPolygon.empty())
        {
            meshBoundaryPolygon.emplace_back(doubleMissingValue, doubleMissingValue);
        }

        // Put the current edge on the mesh boundary, mark it as visited
        const auto startPolygonEdges = meshBoundaryPolygon.size();
        meshBoundaryPolygon.emplace_back(firstNode);
        meshBoundaryPolygon.emplace_back(secondNode);
        isVisited[e] = true;

        // walk the current mesh boundary
        auto currentNode = secondNodeIndex;
        WalkBoundaryFromNode(polygonNodes, isVisited, currentNode, meshBoundaryPolygon);

        const auto numNodesFirstTail = meshBoundaryPolygon.size();

        // if the boundary polygon is not closed
        if (currentNode != firstNodeIndex)
        {
            //Now grow a polyline starting at the other side of the original link L, i.e., the second tail
            currentNode = firstNodeIndex;
            WalkBoundaryFromNode(polygonNodes, isVisited, currentNode, meshBoundaryPolygon);
        }

        // There is a nonempty second tail, so reverse the first tail, so that they connect.
        if (meshBoundaryPolygon.size() > numNodesFirstTail)
        {
            const auto start = startPolygonEdges + static_cast<size_t>(std::ceil((numNodesFirstTail - startPolygonEdges + 1) * 0.5));
            for (auto n = start; n < numNodesFirstTail; n++)
            {
                const auto backupPoint = meshBoundaryPolygon[n];
                const auto replaceIndex = numNodesFirstTail - n + firstNodeIndex;
                meshBoundaryPolygon[n] = meshBoundaryPolygon[replaceIndex];
                meshBoundaryPolygon[replaceIndex] = backupPoint;
            }
        }

        //Start a new polyline
        meshBoundaryPolygon.emplace_back(doubleMissingValue, doubleMissingValue);
    }
    return meshBoundaryPolygon;
}

void meshkernel::Mesh::WalkBoundaryFromNode(const std::vector<Point>& polygonNodes,
                                            std::vector<bool>& isVisited,
                                            size_t& currentNode,
                                            std::vector<Point>& meshBoundaryPolygon) const
{
    size_t e = 0;
    bool currentNodeInPolygon = false;
    while (e < m_nodesNumEdges[currentNode])
    {
        if (!currentNodeInPolygon)
        {
            currentNodeInPolygon = IsPointInPolygonNodes(m_nodes[currentNode], polygonNodes, m_projection);
        }

        if (!currentNodeInPolygon)
        {
            break;
        }

        const auto currentEdge = m_nodesEdges[currentNode][e];
        if (isVisited[currentEdge] || !IsEdgeOnBoundary(currentEdge))
        {
            e++;
            continue;
        }

        currentNode = OtherNodeOfEdge(m_edges[currentEdge], currentNode);
        e = 0;
        currentNodeInPolygon = false;

        meshBoundaryPolygon.emplace_back(m_nodes[currentNode]);
        isVisited[currentEdge] = true;
    }
}

std::vector<size_t> meshkernel::Mesh::GetHangingEdges() const
{
    std::vector<size_t> result;
    for (auto e = 0; e < GetNumEdges(); e++)
    {
        const auto firstNode = m_edges[e].first;
        const auto secondNode = m_edges[e].second;

        if (firstNode != sizetMissingValue && secondNode != sizetMissingValue)
        {
            // if one of the nodes has no other attached edges, the current edge is an hanging edge
            if (m_nodesNumEdges[firstNode] > 1 && m_nodesNumEdges[secondNode] > 1)
            {
                continue;
            }
            result.emplace_back(e);
        }
    }

    return result;
}

void meshkernel::Mesh::DeleteHangingEdges()
{
    const auto hangingEdges = GetHangingEdges();
    for (const auto& hangingEdge : hangingEdges)
    {
        DeleteEdge(hangingEdge);
    }
}