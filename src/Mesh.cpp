#include <vector>
#include <cmath>
#include <numeric>
#include <algorithm>

#include "Mesh.hpp"
#include "Constants.cpp"
#include "Operations.cpp"
#include "Polygons.hpp"
#include "SpatialTrees.hpp"
#include "CurvilinearGrid.hpp"
#include "Entities.hpp"
#include "MakeGridParametersNative.hpp"

bool GridGeom::Mesh::Set(const std::vector<Edge>& edges, 
    const std::vector<Point>& nodes, 
    Projections projection,
    AdministrationOptions administration)
{
    // copy edges and nodes
    m_edges = edges;
    m_nodes = nodes;
    m_projection = projection;

    Administrate(administration);

    //no polygon involved, so node mask is 1 everywhere 
    m_nodeMask.resize(m_nodes.size());
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 1);

    return true;
};

bool GridGeom::Mesh::RemoveInvalidNodesAndEdges()
{
    m_numNodes = 0;
    m_numEdges = 0;

    // Invalidate not connected nodes
    std::vector<bool> connectedNodes(m_nodes.size(), false);
    for (int e = 0; e < m_edges.size(); ++e)
    {
        if (m_edges[e].first < 0 || m_edges[e].second < 0)
        {
            continue;
        }
        int firstNode = m_edges[e].first;
        connectedNodes[firstNode] = true;
        int secondNode = m_edges[e].second;
        connectedNodes[secondNode] = true;
    }
    for (int n = 0; n < m_nodes.size(); ++n)
    {
        if (!connectedNodes[n])
        {
            m_nodes[n] = { doubleMissingValue,doubleMissingValue };
        }
    }

    // Flag invalid nodes
    std::vector<int> validNodesIndexses(m_nodes.size());
    std::fill(validNodesIndexses.begin(), validNodesIndexses.end(), -1);
    int validIndex = 0;
    for (int n = 0; n < m_nodes.size(); ++n)
    {
        if (m_nodes[n].IsValid())
        {
            validNodesIndexses[n] = validIndex;
            validIndex++;
        }
    }

    // Flag invalid edges
    for (int e = 0; e < m_edges.size(); ++e)
    {
        if (m_edges[e].first < 0 || m_edges[e].second < 0)
        {
            continue;
        }

        if (validNodesIndexses[m_edges[e].first] >= 0 && validNodesIndexses[m_edges[e].second] >= 0)
        {
            m_edges[e].first = validNodesIndexses[m_edges[e].first];
            m_edges[e].second = validNodesIndexses[m_edges[e].second];
        }
        else
        {
            m_edges[e].first = -1;
            m_edges[e].second = -1;
        }
    }

    // Remove invalid nodes
    auto endNodeVector = std::remove_if(m_nodes.begin(), m_nodes.end(), [](const Point& n) {return !n.IsValid(); });
    m_numNodes = endNodeVector - m_nodes.begin();

    // Remove invalid edges
    auto endEdgeVector = std::remove_if(m_edges.begin(), m_edges.end(), [&](const Edge& e) {return e.first < 0 || e.second < 0; });
    m_numEdges = endEdgeVector - m_edges.begin();

    return true;
}

bool GridGeom::Mesh::Administrate(AdministrationOptions administrationOption)
{

    RemoveInvalidNodesAndEdges();
    
    // return if there are no nodes or no edges
    if (m_numNodes == 0 || m_numEdges == 0)
    {
        return true;
    }


    ResizeVectorIfNeeded(m_nodes.size(), m_nodesEdges);
    std::fill(m_nodesEdges.begin(), m_nodesEdges.end(), std::vector<int>(maximumNumberOfEdgesPerNode, 0));

    ResizeVectorIfNeeded(m_nodes.size(), m_nodesNumEdges);
    std::fill(m_nodesNumEdges.begin(), m_nodesNumEdges.end(), 0);
       
    NodeAdministration();

    SortEdgesInCounterClockWiseOrder();

    if (administrationOption == AdministrationOptions::AdministrateMeshEdges)
    {
        return true;
    }

    // face administration
    m_numFaces = 0;
    ResizeVectorIfNeeded(m_edges.size(), m_edgesNumFaces);
    std::fill(m_edgesNumFaces.begin(), m_edgesNumFaces.end(), 0);

    ResizeVectorIfNeeded(m_edges.size(), m_edgesFaces);
    std::fill(m_edgesFaces.begin(), m_edgesFaces.end(), std::vector<int>(2, -1));

    m_facesNodes.resize(0);
    m_facesEdges.resize(0);
    m_facesCircumcenters.resize(0);
    m_facesMassCenters.resize(0);
    m_faceArea.resize(0);

    // find faces
    FindFaces();

    // find mesh circumcenters
    ComputeFaceCircumcentersMassCentersAreas();

    // classify node types
    ClassifyNodes();

    return true;
}

//gridtonet
GridGeom::Mesh::Mesh(const CurvilinearGrid& curvilinearGrid, Projections projection)
{

    if (curvilinearGrid.m_grid.size() == 0)
    {
        return;
    }

    std::vector<Point> nodes(curvilinearGrid.m_grid.size()*curvilinearGrid.m_grid[0].size());
    std::vector<Edge>  edges(curvilinearGrid.m_grid.size() * (curvilinearGrid.m_grid[0].size() - 1) + (curvilinearGrid.m_grid.size() - 1) * curvilinearGrid.m_grid[0].size());
    std::vector<std::vector<int>> indexses(curvilinearGrid.m_grid.size(), std::vector<int>(curvilinearGrid.m_grid[0].size(), intMissingValue));

    int ind = 0;
    for (int m = 0; m < curvilinearGrid.m_grid.size(); m++)
    {
        for (int n = 0; n < curvilinearGrid.m_grid[0].size(); n++)
        {
            if (curvilinearGrid.m_grid[m][n].IsValid())
            {
                nodes[ind] = curvilinearGrid.m_grid[m][n];
                indexses[m][n] = ind;
                ind++;
            }
        }
    }
    nodes.resize(ind);

    ind = 0;
    for (int m = 0; m < curvilinearGrid.m_grid.size() - 1; m++)
    {
        for (int n = 0; n < curvilinearGrid.m_grid[0].size(); n++)
        {
            if (indexses[m][n] != intMissingValue && indexses[m + 1][n] != intMissingValue)
            {
                edges[ind].first = indexses[m][n];
                edges[ind].second = indexses[m + 1][n];
                ind++;
            }
        }
    }

    for (int m = 0; m < curvilinearGrid.m_grid.size(); m++)
    {
        for (int n = 0; n < curvilinearGrid.m_grid[0].size() - 1; n++)
        {
            if (indexses[m][n] != intMissingValue && indexses[m][n + 1] != intMissingValue)
            {
                edges[ind].first = indexses[m][n];
                edges[ind].second = indexses[m][n + 1];
                ind++;
            }
        }
    }
    edges.resize(ind);

    Set(edges, nodes, projection, AdministrationOptions::AdministrateMeshEdges);

}

GridGeom::Mesh::Mesh(std::vector<Point>& inputNodes, const GridGeom::Polygons& polygons, Projections projection)
{
    m_projection = projection;
    std::vector<double> xLocalPolygon(inputNodes.size());
    std::vector<double> yLocalPolygon(inputNodes.size());
    for (int i = 0; i < inputNodes.size(); ++i)
    {
        xLocalPolygon[i] = inputNodes[i].x;
        yLocalPolygon[i] = inputNodes[i].y;
    }

    int numtri = -1;
    int jatri = 3;
    int numPointsIn = inputNodes.size();
    int numPointsOut = 0;
    int numberOfTriangles = numPointsIn * 6 + 10;
    double averageTriangleArea = 0.0;
    int numedge = 0;
    std::vector<int> faceNodesFlat;
    std::vector<int> edgeNodesFlat;
    std::vector<int> faceEdgesFlat;
    std::vector<double> xNodesFlat;
    std::vector<double> yNodesFlat;
    // if the number of estimated triangles is not sufficient, triangulation must be repeated
    while (numtri < 0)
    {
        numtri = numberOfTriangles;
        faceNodesFlat.resize(numberOfTriangles * 3);
        edgeNodesFlat.resize(numberOfTriangles * 2);
        faceEdgesFlat.resize(numberOfTriangles * 3);
        xNodesFlat.resize(numberOfTriangles * 3, doubleMissingValue);
        yNodesFlat.resize(numberOfTriangles * 3, doubleMissingValue);
        Triangulation(&jatri,
            &xLocalPolygon[0],
            &yLocalPolygon[0],
            &numPointsIn,
            &faceNodesFlat[0],   // INDX
            &numtri,
            &edgeNodesFlat[0],   // EDGEINDX
            &numedge,
            &faceEdgesFlat[0],   // TRIEDGE
            &xNodesFlat[0],
            &yNodesFlat[0],
            &numPointsOut,
            &averageTriangleArea);
        if (numberOfTriangles)
        {
            numberOfTriangles = -numtri;
        }
    }

    // create face nodes
    std::vector<std::vector<int>> faceNodes(numtri, std::vector<int>(3, -1));
    std::vector<std::vector<int>> faceEdges(numtri, std::vector<int>(3, -1));
    int index = 0;
    for (int i = 0; i < numtri; ++i)
    {
        faceNodes[i][0] = faceNodesFlat[index] - 1;
        faceEdges[i][0] = faceEdgesFlat[index] - 1;
        index++;
        faceNodes[i][1] = faceNodesFlat[index] - 1;
        faceEdges[i][1] = faceEdgesFlat[index] - 1;
        index++;
        faceNodes[i][2] = faceNodesFlat[index] - 1;
        faceEdges[i][2] = faceEdgesFlat[index] - 1;
        index++;
    }

    // create edges
    std::vector<std::vector<int>> edgeNodes(numedge, std::vector<int>(2, 0));
    index = 0;
    for (int i = 0; i < numedge; ++i)
    {
        edgeNodes[i][0] = edgeNodesFlat[index] - 1;
        index++;
        edgeNodes[i][1] = edgeNodesFlat[index] - 1;
        index++;
    }


    // for each triangle we have to check
    // 1. validity of its internal angles
    // 2. is inside the polygon
    // if so we mark the edges and we add them to kn table
    std::vector<bool> edgeNodesFlag(numedge, false);
    for (int i = 0; i < numtri; ++i)
    {
        bool goodTriangle = CheckTriangle(faceNodes[i], inputNodes);

        if (!goodTriangle)
        {
            continue;
        }
        Point approximateCenter = (inputNodes[faceNodes[i][0]] + inputNodes[faceNodes[i][1]] + inputNodes[faceNodes[i][2]]) * oneThird;

        bool isTriangleInPolygon = polygons.IsPointInPolygon(approximateCenter,0);
        if (!isTriangleInPolygon)
        {
            continue;
        }

        // mark all edges of this triangle as good ones
        for (int j = 0; j < 3; ++j)
        {
            edgeNodesFlag[faceEdges[i][j]] = true;
        }
    }

    // now add all points and all valid edges
    m_nodes = inputNodes;
    int validEdgesCount = 0;
    for (int i = 0; i < numedge; ++i)
    {
        if (!edgeNodesFlag[i])
            continue;
        validEdgesCount++;
    }

    std::vector<Edge> edges(validEdgesCount);
    validEdgesCount = 0;
    for (int i = 0; i < numedge; ++i)
    {
        if (!edgeNodesFlag[i])
            continue;

        edges[validEdgesCount].first = std::abs(edgeNodes[i][0]);
        edges[validEdgesCount].second = edgeNodes[i][1];
        validEdgesCount++;
    }

    Set(edges, inputNodes, projection, AdministrationOptions::AdministrateMeshEdges);

}

bool GridGeom::Mesh::CheckTriangle(const std::vector<int>& faceNodes, const std::vector<Point>& nodes)
{
    double phiMin = 1e3;
    double phiMax = 0.0;
    static std::vector<std::vector<int>> nodePermutations
    {
        {2,0,1}, {0,1,2}, {1,2,0}
    };

    for (int i = 0; i < faceNodes.size(); ++i)
    {
        Point x0 = nodes[faceNodes[nodePermutations[i][0]]];
        Point x1 = nodes[faceNodes[nodePermutations[i][1]]];
        Point x2 = nodes[faceNodes[nodePermutations[i][2]]];

        double cosphi = NormalizedInnerProductTwoSegments(x1, x0, x1, x2, m_projection);
        double phi = std::acos(std::min(std::max(cosphi, -1.0), 1.0)) * raddeg_hp;
        phiMin = std::min(phiMin, phi);
        phiMax = std::max(phiMax, phi);
        if (phi < m_triangleMinimumAngle || phi > m_triangleMaximumAngle)
        {
            return false;
        }
    }
    return true;
}


bool GridGeom::Mesh::SetFlatCopies(AdministrationOptions administrationOption)
{
    Administrate(administrationOption);

    m_nodex.resize(GetNumNodes());
    m_nodey.resize(GetNumNodes());
    m_nodez.resize(GetNumNodes());
    for (int n = 0; n < GetNumNodes(); n++)
    {
        m_nodex[n] = m_nodes[n].x;
        m_nodey[n] = m_nodes[n].y;
        m_nodez[n] = 0.0;
    }

    int edgeIndex = 0;
    m_edgeNodes.resize(GetNumEdges() * 2);
    for (int e = 0; e < GetNumEdges(); e++)
    {
        m_edgeNodes[edgeIndex] = m_edges[e].first;
        edgeIndex++;
        m_edgeNodes[edgeIndex] = m_edges[e].second;
        edgeIndex++;
    }

    int faceIndex = 0;
    m_faceNodes.resize(GetNumFaces() * maximumNumberOfNodesPerFace, intMissingValue);
    m_facesCircumcentersx.resize(GetNumFaces());
    m_facesCircumcentersy.resize(GetNumFaces());
    m_facesCircumcentersz.resize(GetNumFaces());
    for (int f = 0; f < GetNumFaces(); f++)
    {
        for (int n = 0; n < maximumNumberOfNodesPerFace; ++n)
        {
            if (n < m_facesNodes[f].size())
            {
                m_faceNodes[faceIndex] = m_facesNodes[f][n];
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


    return true;
}

void GridGeom::Mesh::NodeAdministration()
{
    // assume no duplicated links
    for (int e = 0; e < GetNumEdges(); e++)
    {
        const auto firstNode = m_edges[e].first;
        const auto secondNode = m_edges[e].second;

        if (firstNode < 0 || secondNode < 0)
        {
            continue;
        }

        if(m_nodesNumEdges[firstNode]>= maximumNumberOfEdgesPerNode || m_nodesNumEdges[secondNode] >= maximumNumberOfEdgesPerNode)
        {
            continue; 
        }

        // Search for previously connected edges
        bool alreadyAddedEdge = false;
        for (int i = 0; i < m_nodesNumEdges[firstNode]; ++i)
        {
            auto currentEdge = m_edges[m_nodesEdges[firstNode][i]];
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
        for (int i = 0; i < m_nodesNumEdges[secondNode]; ++i)
        {
            auto currentEdge = m_edges[m_nodesEdges[secondNode][i]];
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


void GridGeom::Mesh::SortEdgesInCounterClockWiseOrder()
{
    std::vector<double> edgesAngles(GridGeom::maximumNumberOfEdgesPerNode, 0.0);
    for (auto node = 0; node < GetNumNodes(); node++)
    {
        if (!m_nodes[node].IsValid())
        {
            continue;
        }

        double phi0 = 0.0;
        double phi;
        std::fill(edgesAngles.begin(), edgesAngles.end(), 0.0);
        for (auto edgeIndex = 0; edgeIndex < m_nodesNumEdges[node]; edgeIndex++)
        {

            auto firstNode = m_edges[m_nodesEdges[node][edgeIndex]].first;
            auto secondNode = m_edges[m_nodesEdges[node][edgeIndex]].second;
            if (firstNode < 0 || secondNode < 0)
            {
                continue;
            }

            if (secondNode == node)
            {
                secondNode = firstNode;
                firstNode = node;
            }

            double deltaX = GetDx(m_nodes[secondNode], m_nodes[firstNode], m_projection);
            double deltaY = GetDy(m_nodes[secondNode], m_nodes[firstNode], m_projection);
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

            edgesAngles[edgeIndex] = phi - phi0;
            if (edgesAngles[edgeIndex] < 0.0)
            {
                edgesAngles[edgeIndex] = edgesAngles[edgeIndex] + 2.0 * M_PI;
            }
        }

        // Performing sorting
        std::vector<std::size_t> indexes(m_nodesNumEdges[node]);
        std::vector<int> edgeNodeCopy{ m_nodesEdges[node] };
        iota(indexes.begin(), indexes.end(), 0);
        sort(indexes.begin(), indexes.end(), [&edgesAngles](std::size_t i1, std::size_t i2) {return edgesAngles[i1] < edgesAngles[i2]; });

        for (std::size_t edgeIndex = 0; edgeIndex < m_nodesNumEdges[node]; edgeIndex++)
        {
            m_nodesEdges[node][edgeIndex] = edgeNodeCopy[indexes[edgeIndex]];
        }
    }
}

// look at sub_findelemcontours in IrregularGridClass.f90 for a similar implementation
bool GridGeom::Mesh::FindFacesRecursive(
    int startingNode,
    int node,
    int index,
    int previusEdge,
    std::vector<int>& edges,
    std::vector<int>& nodes,
    std::vector<int>& sortedEdgesFaces,
    std::vector<int>& sortedNodes)
{
    if (index >= edges.size())
    {
        return false;
    }

    if (m_edgesNumFaces[previusEdge] >= 2)
    {
        return false;
    }

    if (m_edges[previusEdge].first < 0 || m_edges[previusEdge].second < 0)
    {
        return false;
    }

    edges[index] = previusEdge;
    nodes[index] = node;
    const int otherNode = m_edges[previusEdge].first + m_edges[previusEdge].second - node;

    // enclosure found
    if (otherNode == startingNode && index == edges.size() - 1)
    {
        // all nodes must be unique
        sortedNodes = nodes;
        std::sort(sortedNodes.begin(), sortedNodes.end());
        for (int n = 0; n < sortedNodes.size() - 1; n++)
        {
            if (sortedNodes[n + 1] == sortedNodes[n])
            {
                return false;
            }
        }
        // we need to add a face when at least one edge has no faces
        bool oneEdgeHasNoFace = false;
        for (int ee = 0; ee < edges.size(); ee++)
        {
            if (m_edgesNumFaces[edges[ee]] == 0)
            {
                oneEdgeHasNoFace = true;
                break;
            }
        }

        if (!oneEdgeHasNoFace)
        {
            // is an internal face only if all edges have a different face
            for (int ee = 0; ee < edges.size(); ee++)
            {
                sortedEdgesFaces[ee] = m_edgesFaces[edges[ee]][0];
            }
            std::sort(sortedEdgesFaces.begin(), sortedEdgesFaces.end());
            for (int n = 0; n < sortedEdgesFaces.size() - 1; n++)
            {
                if (sortedEdgesFaces[n + 1] == sortedEdgesFaces[n])
                {
                    return false;
                }
            }
        }

        // increase m_edgesNumFaces 
        m_numFaces += 1;
        for (int ee = 0; ee < edges.size(); ee++)
        {
            m_edgesNumFaces[edges[ee]] += 1;
            const int numFace = m_edgesNumFaces[edges[ee]];
            m_edgesFaces[edges[ee]][numFace - 1] = m_numFaces - 1;
        }

        // store the result
        m_facesNodes.push_back(nodes);
        m_facesEdges.push_back(edges);
        return true;
    }

    int edgeIndexOtherNode = 0;
    for (int e = 0; e < m_nodesNumEdges[otherNode]; e++)
    {
        if (m_nodesEdges[otherNode][e] == previusEdge)
        {
            edgeIndexOtherNode = e;
            break;
        }
    }

    edgeIndexOtherNode = edgeIndexOtherNode - 1;
    if (edgeIndexOtherNode < 0)
    {
        edgeIndexOtherNode = edgeIndexOtherNode + m_nodesNumEdges[otherNode];
    }
    if (edgeIndexOtherNode > m_nodesNumEdges[otherNode] - 1)
    {
        edgeIndexOtherNode = edgeIndexOtherNode - m_nodesNumEdges[otherNode];
    }

    const int edge = m_nodesEdges[otherNode][edgeIndexOtherNode];
    FindFacesRecursive(startingNode, otherNode, index + 1, edge, edges, nodes, sortedEdgesFaces, sortedNodes);


    return true;

}

void GridGeom::Mesh::FindFaces()
{
    for (int numEdgesPerFace = 3; numEdgesPerFace <= maximumNumberOfEdgesPerFace; numEdgesPerFace++)
    {
        std::vector<int> edges(numEdgesPerFace);
        std::vector<int> nodes(numEdgesPerFace);
        std::vector<int> sortedEdgesFaces(numEdgesPerFace);
        std::vector<int> sortedNodes(numEdgesPerFace);
        for (int n = 0; n < GetNumNodes(); n++)
        {
            if (!m_nodes[n].IsValid())
            {
                continue;
            }

            for (int e = 0; e < m_nodesNumEdges[n]; e++)
            {
                FindFacesRecursive(n, n, 0, m_nodesEdges[n][e], edges, nodes, sortedEdgesFaces, sortedNodes);
            }
        }
    }

    m_numFacesNodes.resize(m_numFaces);
    for (int f = 0; f < m_numFaces; ++f)
    {
        m_numFacesNodes[f] = m_facesNodes[f].size();
    }
}

void GridGeom::Mesh::ComputeFaceCircumcentersMassCentersAreas()
{
    m_facesCircumcenters.resize(GetNumFaces());
    m_faceArea.resize(GetNumFaces());
    m_facesMassCenters.resize(GetNumFaces());

    std::vector<Point> middlePointsCache(maximumNumberOfNodesPerFace);
    std::vector<Point> normalsCache(maximumNumberOfNodesPerFace);
    std::vector<int> numEdgeFacesCache(maximumNumberOfEdgesPerFace);
    m_polygonNodesCache.resize(maximumNumberOfNodesPerFace + 1);
    for (int f = 0; f < GetNumFaces(); f++)
    {
        //need to account for spherical coordinates. Build a polygon around a face
        int numPolygonPoints;
        bool successful = FaceClosedPolygon(f, m_polygonNodesCache, numPolygonPoints);
        if (!successful)
        {
            return;
        }

        auto numberOfFaceNodes = GetNumFaceEdges(f);
        double area;
        Point centerOfMass;
        successful = FaceAreaAndCenterOfMass(m_polygonNodesCache, numberOfFaceNodes, m_projection, area, centerOfMass);
        if (!successful)
        {
            return;
        }

        m_faceArea[f] = area;
        m_facesMassCenters[f] = centerOfMass;

        int numberOfInteriorEdges = 0;
        for (int n = 0; n < numberOfFaceNodes; n++)
        {
            if (m_edgesNumFaces[m_facesEdges[f][n]] == 2)
            {
                numberOfInteriorEdges += 1;
            }
        }
        if (numberOfInteriorEdges == 0)
        {
            m_facesCircumcenters[f] = centerOfMass;
            continue;
        }

        for (int n = 0; n < numberOfFaceNodes; n++)
        {
            numEdgeFacesCache[n] = m_edgesNumFaces[m_facesEdges[f][n]];
        }

        successful = ComputePolygonCircumenter(m_polygonNodesCache,
            middlePointsCache,
            normalsCache,
            numberOfFaceNodes,
            numEdgeFacesCache,
            m_projection,
            weightCircumCenter,
            m_facesCircumcenters[f]);

        if (!successful)
        {
            return;
        }
    }
}

bool GridGeom::Mesh::ClassifyNodes()
{
    m_nodesTypes.resize(GetNumNodes(), 0);
    std::fill(m_nodesTypes.begin(), m_nodesTypes.end(), 0);

    for (int e = 0; e < GetNumEdges(); e++)
    {
        const auto firstNode = m_edges[e].first;
        const auto secondNode = m_edges[e].second;

        if (firstNode < 0 || secondNode < 0)
        {
            continue;
        }

        if (m_edgesNumFaces[e] == 0)
        {
            m_nodesTypes[firstNode] = -1;
            m_nodesTypes[secondNode] = -1;
        }
        else if (m_edgesNumFaces[e] == 1)
        {
            m_nodesTypes[firstNode] += 1;
            m_nodesTypes[secondNode] += 1;
        }
    }

    for (int n = 0; n < GetNumNodes(); n++)
    {
        if (m_nodesTypes[n] == 1 || m_nodesTypes[n] == 2)
        {
            if (m_nodesNumEdges[n] == 2)
            {
                //corner point
                m_nodesTypes[n] = 3;
            }
            else {}
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
    return true;
}


bool GridGeom::Mesh::MakeMesh(const GridGeomApi::MakeGridParametersNative& makeGridParametersNative, const Polygons& polygons)
{
    CurvilinearGrid CurvilinearGrid;
    m_projection = polygons.m_projection;
    if (makeGridParametersNative.GridType == 0)
    {
        // regular grid
        int numM = makeGridParametersNative.NumberOfColumns + 1;
        int numN = makeGridParametersNative.NumberOfRows + 1;
        double XGridBlockSize = makeGridParametersNative.XGridBlockSize;
        double YGridBlockSize = makeGridParametersNative.YGridBlockSize;
        double cosineAngle = std::cos(makeGridParametersNative.GridAngle*degrad_hp);
        double sinAngle = std::sin(makeGridParametersNative.GridAngle*degrad_hp);
        double OriginXCoordinate = makeGridParametersNative.OriginXCoordinate;
        double OriginYCoordinate = makeGridParametersNative.OriginYCoordinate;

        // in case a polygon is there, re-compute parameters
        if (polygons.m_numNodes >= 3)
        {
            Point referencePoint;
            // rectangular grid in polygon
            for (int i = 0; i < polygons.m_numNodes; ++i)
            {
                if (polygons.m_nodes[i].IsValid())
                {
                    referencePoint = polygons.m_nodes[i];
                    break;
                }
            }

            // get polygon min/max in rotated (xi,eta) coordinates
            double xmin = std::numeric_limits<double>::max();
            double xmax = -xmin;
            double etamin = std::numeric_limits<double>::max();
            double etamax = -etamin;
            for (int i = 0; i < polygons.m_numNodes; ++i)
            {
                if (polygons.m_nodes[i].IsValid())
                {
                    double dx = GetDx(referencePoint, polygons.m_nodes[i], m_projection);
                    double dy = GetDy(referencePoint, polygons.m_nodes[i], m_projection);
                    double xi = dx * cosineAngle + dy * sinAngle;
                    double eta = -dx * sinAngle + dy * cosineAngle;
                    xmin = std::min(xmin, xi);
                    xmax = std::max(xmax, xi);
                    etamin = std::min(etamin, eta);
                    etamax = std::max(etamax, eta);
                }
            }

            double xShift = xmin*cosineAngle - etamin * sinAngle;
            double yShift = xmin*sinAngle + etamin* cosineAngle;
            if (m_projection == Projections::spherical)
            {
                xShift = xShift / earth_radius *raddeg_hp;
                yShift = yShift / (earth_radius *std::cos(referencePoint.y*degrad_hp)) * raddeg_hp;
            }

            OriginXCoordinate = referencePoint.x + xShift;
            OriginYCoordinate = referencePoint.y + yShift;
            numN = std::ceil((etamax - etamin) / XGridBlockSize) + 1;
            numM = std::ceil((xmax - xmin) / YGridBlockSize) + 1;
        }


        CurvilinearGrid.IncreaseGrid(numN, numM);
        for (int n = 0; n < numN; ++n)
        {
            for (int m = 0; m < numM; ++m)
            {
                double newPointXCoordinate = OriginXCoordinate + m * XGridBlockSize * cosineAngle - n * YGridBlockSize * sinAngle;
                double newPointYCoordinate = OriginYCoordinate + m * XGridBlockSize * sinAngle + n * YGridBlockSize * cosineAngle;
                if(m_projection==Projections::spherical && n > 0)
                {
                    newPointYCoordinate = XGridBlockSize * cos(degrad_hp * CurvilinearGrid.m_grid[n - 1][m].y);
                }
                CurvilinearGrid.m_grid[n][m] = { newPointXCoordinate , newPointYCoordinate };
            }
        }

        // in case a polygon is there, remove nodes outside
        if (polygons.m_numNodes >= 3)
        {
            std::vector<std::vector<bool>> nodeBasedMask(numN, std::vector<bool>(numM, false));
            std::vector<std::vector<bool>> faceBasedMask(numN - 1, std::vector<bool>(numM - 1, false));
            // mark points inside a polygon
            for (int n = 0; n < numN; ++n)
            {
                for (int m = 0; m < numM; ++m)
                {
                    bool isInPolygon = polygons.IsPointInPolygon(CurvilinearGrid.m_grid[n][m],0);
                    if (isInPolygon)
                    {
                        nodeBasedMask[n][m] = true;
                    }
                }
            }

            // mark faces when at least one node is inside
            for (int n = 0; n < numN - 1; ++n)
            {
                for (int m = 0; m < numM - 1; ++m)
                {
                    if (nodeBasedMask[n][m] || nodeBasedMask[n + 1][m] || nodeBasedMask[n][m + 1] || nodeBasedMask[n + 1][m + 1])
                    {
                        faceBasedMask[n][m] = true;
                    }
                }
            }

            //mark nodes that are member of a cell inside the polygon(s)
            for (int n = 0; n < numN - 1; ++n)
            {
                for (int m = 0; m < numM - 1; ++m)
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
            for (int n = 0; n < numN; ++n)
            {
                for (int m = 0; m < numM; ++m)
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

    Administrate(AdministrationOptions::AdministrateMeshEdges);

    return true;
}

///MERGENODESINPOLYGON
bool GridGeom::Mesh::MergeNodesInPolygon(const Polygons& polygon)
{
    // first filter the nodes in polygon
    std::vector<Point> filteredNodes(GetNumNodes());
    std::vector<int> originalNodeIndexses(GetNumNodes(), - 1);
    int index = 0;
    for (int i = 0; i < GetNumNodes(); i++)
    {
        bool inPolygon = polygon.IsPointInPolygon(m_nodes[i],0);
        if (inPolygon)
        {
            filteredNodes[index] = m_nodes[i];
            originalNodeIndexses[index] = i;
            index++;
        }
    }
    filteredNodes.resize(index);

    // Update the R-Tree of the mesh nodes
    m_nodesRTree.Clear();
    m_nodesRTree.BuildTree(filteredNodes, m_projection);
    
    // merge the closest nodes
    for (int i = 0; i < filteredNodes.size(); i++)
    {
        m_nodesRTree.NearestNeighbours(filteredNodes[i], mergingDistance);

        int resultSize = m_nodesRTree.GetQueryResultSize();
        if (resultSize > 1)
        {
            for (int j = 0; j < m_nodesRTree.GetQueryResultSize(); j++)
            {
                auto nodeIndexInFilteredNodes = m_nodesRTree.GetQuerySampleIndex(j);
                if (nodeIndexInFilteredNodes != i)
                {
                    MergeTwoNodes(originalNodeIndexses[i], originalNodeIndexses[nodeIndexInFilteredNodes]);
                    m_nodesRTree.RemoveNode(i);
                }
            }
        }

    }

    Administrate(AdministrationOptions::AdministrateMeshEdges);

    return true;
}

///mergenodes
bool GridGeom::Mesh::MergeTwoNodes(int firstNodeIndex, int secondNodeIndex)
{
    if(firstNodeIndex>=GetNumNodes() || secondNodeIndex >= GetNumNodes())
    {
        return true;
    }
    
    int edgeIndex;
    FindEdge(firstNodeIndex, secondNodeIndex, edgeIndex);
    if (edgeIndex >= 0)
    {
        m_edges[edgeIndex].first = -1;
        m_edges[edgeIndex].second = -1;
    }

    // check if there is another edge starting at firstEdgeOtherNode and ending at secondNode
    for (auto n = 0; n < m_nodesNumEdges[firstNodeIndex]; n++)
    {
        auto firstEdgeIndex = m_nodesEdges[firstNodeIndex][n];
        auto firstEdge = m_edges[firstEdgeIndex];
        auto firstEdgeOtherNode = firstEdge.first + firstEdge.second - firstNodeIndex;
        if (firstEdgeOtherNode >= 0 && firstEdgeOtherNode != secondNodeIndex)
        {
            for (int nn = 0; nn < m_nodesNumEdges[firstEdgeOtherNode]; nn++)
            {
                auto secondEdgeIndex = m_nodesEdges[firstEdgeOtherNode][nn];
                auto secondEdge = m_edges[secondEdgeIndex];
                auto secondNodeSecondEdge = secondEdge.first + secondEdge.second - firstEdgeOtherNode;
                if (secondNodeSecondEdge == secondNodeIndex)
                {
                    m_edges[secondEdgeIndex].first = -1;
                    m_edges[secondEdgeIndex].second = -1;
                }
            }
        }
    }

    // add all valid edges starting at secondNode
    std::vector<int> secondNodeEdges(maximumNumberOfEdgesPerNode,-1);
    int numSecondNodeEdges = 0;
    for (auto n = 0; n < m_nodesNumEdges[secondNodeIndex]; n++)
    {
        auto edgeIndex = m_nodesEdges[secondNodeIndex][n];
        if (m_edges[edgeIndex].first >= 0)
        {
            secondNodeEdges[numSecondNodeEdges] = edgeIndex;
            numSecondNodeEdges++;
        }
    }

    // add all valid edges starting at firstNode are assigned to the second node
    for (auto n = 0; n < m_nodesNumEdges[firstNodeIndex]; n++)
    {
        auto edgeIndex = m_nodesEdges[firstNodeIndex][n];
        if (m_edges[edgeIndex].first >= 0)
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
    m_nodesEdges[secondNodeIndex] = std::move(std::vector<int>(secondNodeEdges.begin(), secondNodeEdges.begin() + numSecondNodeEdges));
    m_nodesNumEdges[secondNodeIndex] = numSecondNodeEdges;

    // remove edges to first node
    m_nodesEdges[firstNodeIndex] = std::move(std::vector<int>(0));
    m_nodesNumEdges[firstNodeIndex] = 0;
    m_nodes[firstNodeIndex] = { doubleMissingValue, doubleMissingValue };

    return true;
}

bool GridGeom::Mesh::ConnectNodes(int startNode, int endNode, int& newEdgeIndex)
{
    int edgeIndex;
    bool successful = FindEdge(startNode, endNode, edgeIndex);
    if (!successful)
    {
        return false;
    }
    if (edgeIndex >= 0)
    {
        return true;
    }

    // increment the edges container
    newEdgeIndex = GetNumEdges();
    ResizeVectorIfNeeded(newEdgeIndex + 1, m_edges, std::make_pair(intMissingValue,intMissingValue));
    m_edges[newEdgeIndex].first = startNode;
    m_edges[newEdgeIndex].second = endNode;
    m_numEdges++;

    return true;
}

bool GridGeom::Mesh::InsertNode(const Point& newPoint, int& newNodeIndex, bool updateRTree)
{
    int newSize = GetNumNodes() + 1;
    newNodeIndex = GetNumNodes();

    ResizeVectorIfNeeded(newSize, m_nodes);
    ResizeVectorIfNeeded(newSize, m_nodeMask);
    ResizeVectorIfNeeded(newSize, m_nodesNumEdges);
    ResizeVectorIfNeeded(newSize, m_nodesEdges);
    m_numNodes++;

    m_nodes[newNodeIndex] = newPoint;
    m_nodeMask[newNodeIndex] = newNodeIndex;
    m_nodesNumEdges[newNodeIndex] = 0;

    if(updateRTree)
    {
        RefreshRTreeIfNeeded();
    }

    return true;
}

bool GridGeom::Mesh::DeleteNode(int nodeIndex, bool updateRTree)
{
    if(nodeIndex>=GetNumNodes())
    {
        return true;
    }

    for (int e = 0; e <  m_nodesNumEdges[nodeIndex]; e++)
    {
        auto edgeIndex = m_nodesEdges[nodeIndex][e];
        DeleteEdge(edgeIndex);
    }
    m_nodes[nodeIndex] = { doubleMissingValue,doubleMissingValue };
    m_numNodes--;

    if (updateRTree)
    {
        RefreshRTreeIfNeeded();
        m_nodesRTree.RemoveNode(nodeIndex);
    }

    return true;
}

bool GridGeom::Mesh::RefreshRTreeIfNeeded()
{
    if (m_nodesRTree.Empty())
    {
        m_nodesRTree.BuildTree(m_nodes, m_projection);
    }

    //insert the missing nodes
    if (m_nodesRTree.Size() < GetNumNodes())
    {
        for (int i = m_nodesRTree.Size(); i < GetNumNodes(); ++i)
        {
            m_nodesRTree.InsertNode(m_nodes[i]);
        }
    }
    return true;
}

bool GridGeom::Mesh::DeleteEdge(int edgeIndex)
{
    if(edgeIndex<0)
    {
        return true;
    }

    m_edges[edgeIndex].first = intMissingValue;
    m_edges[edgeIndex].second = intMissingValue;

    return true;
}


bool GridGeom::Mesh::FaceClosedPolygon(int faceIndex, std::vector<Point>& polygonNodesCache, int& numClosedPolygonNodes) const
{
    auto numFaceNodes = GetNumFaceEdges(faceIndex);
    if (polygonNodesCache.size() < numFaceNodes + 1)
    {
        polygonNodesCache.resize(numFaceNodes + 1);
    }

    for (int n = 0; n < numFaceNodes; n++)
    {
        polygonNodesCache[n] = m_nodes[m_facesNodes[faceIndex][n]];
    }
    polygonNodesCache[numFaceNodes] = polygonNodesCache[0];

    numClosedPolygonNodes = numFaceNodes + 1;

    return true;
}


bool GridGeom::Mesh::FaceClosedPolygon(int faceIndex, std::vector<Point>& polygonNodesCache, 
    std::vector<int>& localNodeIndexsesCache, 
    std::vector<int>& edgeIndexsesCache,
    int& numClosedPolygonNodes) const
{
    auto numFaceNodes = GetNumFaceEdges(faceIndex);
    if (polygonNodesCache.size() < numFaceNodes + 1)
    {
        polygonNodesCache.resize(numFaceNodes + 1);
    }

    if (localNodeIndexsesCache.size() < numFaceNodes + 1)
    {
        localNodeIndexsesCache.resize(numFaceNodes + 1);
    }

    if (edgeIndexsesCache.size() < numFaceNodes + 1)
    {
        edgeIndexsesCache.resize(numFaceNodes + 1);
    }

    for (int n = 0; n < numFaceNodes; n++)
    {
        polygonNodesCache[n] = m_nodes[m_facesNodes[faceIndex][n]];
        localNodeIndexsesCache[n] = n;
        edgeIndexsesCache[n] = m_facesEdges[faceIndex][n];
    }
    polygonNodesCache[numFaceNodes] = polygonNodesCache[0];
    localNodeIndexsesCache[numFaceNodes] = 0;
    edgeIndexsesCache[numFaceNodes] = m_facesEdges[faceIndex][0];
    numClosedPolygonNodes = numFaceNodes + 1;

    return true;
}


bool GridGeom::Mesh::MaskNodesInPolygons(const Polygons& polygon, bool inside)
{
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 0);
    for (int i = 0; i < GetNumNodes(); ++i)
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

    return true;
}

bool GridGeom::Mesh::ComputeEdgeLengths()
{
    auto numEdges = GetNumEdges();
    m_edgeLengths.resize(numEdges, doubleMissingValue);
    for (int e = 0; e < numEdges; e++)
    {
        int first = m_edges[e].first;
        int second = m_edges[e].second;
        m_edgeLengths[e] = Distance(m_nodes[first], m_nodes[second], m_projection);
    }
    return true;
}

bool GridGeom::Mesh::IsFullFaceNotInPolygon(int faceIndex) const
{
    for (int n = 0; n < GetNumFaceEdges(faceIndex); n++)
    {
        if (m_nodeMask[m_facesNodes[faceIndex][n]] != 1)
        {
            return true;
        }
    }
    return false;
}

bool GridGeom::Mesh::FindCommonNode(int firstEdgeIndex, int secondEdgeIndex, int& node) const
{
    auto firstEdgeFirstNode = m_edges[firstEdgeIndex].first;
    auto firstEdgeEdgeSecondNode = m_edges[firstEdgeIndex].second;

    auto secondEdgeFirstNode = m_edges[secondEdgeIndex].first;
    auto secondEdgeSecondNode = m_edges[secondEdgeIndex].second;

    if (firstEdgeFirstNode < 0 || firstEdgeEdgeSecondNode < 0 || secondEdgeFirstNode < 0 || secondEdgeSecondNode < 0)
    {
        return false;
    }

    if (firstEdgeFirstNode == secondEdgeFirstNode || firstEdgeFirstNode == secondEdgeSecondNode)
    {
        node = firstEdgeFirstNode;
        return true;
    }

    if (firstEdgeEdgeSecondNode == secondEdgeFirstNode || firstEdgeEdgeSecondNode == secondEdgeSecondNode)
    {
        node = firstEdgeEdgeSecondNode;
        return true;
    }

    return true;
}

bool GridGeom::Mesh::FindEdge(int firstNodeIndex, int secondNodeIndex, int& edgeIndex) const
{
    if (firstNodeIndex < 0 || secondNodeIndex < 0)
    {
        return false;
    }

    edgeIndex = -1;
    for (auto n = 0; n < m_nodesNumEdges[firstNodeIndex]; n++)
    {
        int localEdgeIndex = m_nodesEdges[firstNodeIndex][n];
        auto firstEdgeOtherNode = m_edges[localEdgeIndex].first + m_edges[localEdgeIndex].second - firstNodeIndex;
        if (firstEdgeOtherNode == secondNodeIndex)
        {
            edgeIndex = localEdgeIndex;
            break;
        }
    }
    return true;
}

bool GridGeom::Mesh::GetBoundingBox(Point& lowerLeft, Point& upperRight) const
{

    double minx = std::numeric_limits<double>::max();
    double maxx = std::numeric_limits<double>::min();
    double miny = std::numeric_limits<double>::max();
    double maxy = std::numeric_limits<double>::min();
    for (int n = 0; n < GetNumNodes(); n++)
    {
        if (m_nodes[n].IsValid())
        {
            minx = std::min(minx, m_nodes[n].x);
            maxx = std::max(maxx, m_nodes[n].x);
            miny = std::min(miny, m_nodes[n].y);
            maxy = std::max(maxy, m_nodes[n].y);
        }
    }
    lowerLeft = { minx , miny };
    upperRight = { maxx , maxy };

    return true;
}

bool GridGeom::Mesh::OffsetSphericalCoordinates(double minx, double maxx)
{
    if(m_projection==Projections::spherical && maxx - minx > 180.0)
    {
        for (int n = 0; n < GetNumNodes(); ++n)
        {
            if(m_nodes[n].x-360.0 >= minx)
            {
                m_nodes[n].x -= 360.0;
            }

            if (m_nodes[n].x < minx)
            {
                m_nodes[n].x += 360.0;
            }
        }
    }

    return true;
}


bool GridGeom::Mesh::GetNodeIndex(Point point, double searchRadius, int& vertexIndex)
{
    if (GetNumNodes() == 0)
    {
        return true;
    }

    double closestDistance = std::numeric_limits<double>::max();
    for (int n = 0; n < GetNumNodes(); ++n)
    {
        const auto absDx = std::abs(GetDx(m_nodes[n], point, m_projection));
        const auto absDy = std::abs(GetDy(m_nodes[n], point, m_projection));
        if (absDx < searchRadius && absDy < searchRadius)
        {
            const double squaredDistance = ComputeSquaredDistance(m_nodes[n], point, m_projection);
            if (squaredDistance < closestDistance)
            {
                closestDistance = squaredDistance;
                vertexIndex = n;
            }
        }
    }

    return true;
}

bool GridGeom::Mesh::DeleteEdgeCloseToAPoint(Point point, double searchRadius)
{
    // linear search of the closest edge. The alternative is to mantain an rtree also for edge centers
    int edgeIndex = -1;
    double closestDistance = std::numeric_limits<double>::max();
    for (int e = 0; e < GetNumEdges(); ++e)
    {
        const auto firstNode = m_edges[e].first;
        const auto secondNode = m_edges[e].second;

        if (firstNode < 0 || secondNode < 0)
        {
            continue;
        }

        auto edgeCenter = (m_nodes[firstNode] + m_nodes[secondNode]) / 2.0;
        const auto absDx = std::abs(GetDx(point, edgeCenter, m_projection));
        const auto absDy = std::abs(GetDy(point, edgeCenter, m_projection));

        if (absDx < searchRadius && absDy < searchRadius)
        {
            const double squaredDistance = ComputeSquaredDistance(point, edgeCenter, m_projection);
            if (squaredDistance < closestDistance)
            {
                closestDistance = squaredDistance;
                edgeIndex = e;
            }
        }
    }

    if(edgeIndex==-1)
    {
        return true;
    }

    bool successful = DeleteEdge(edgeIndex);

    if(!successful)
    {
        return false;
    }

    return true;
}

bool GridGeom::Mesh::MaskFaceEdgesInPolygon(const Polygons& polygons, bool invertMasking, bool includeIntersected)
{
    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);

    // mark all nodes in polygon with 1
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 0);
    for (int n = 0; n < GetNumNodes(); ++n)
    {
        auto isInPolygon = polygons.IsPointInPolygon(m_nodes[n], 0);
        if (isInPolygon)
        {
            m_nodeMask[n] = 1;
        }
    }

    // mark all edges with both start end end nodes included with 1
    std::vector<int> edgeMask(m_edges.size(), 0);
    for (int e = 0; e < GetNumEdges(); ++e)
    {
        auto firstNodeIndex = m_edges[e].first;
        auto secondNodeIndex = m_edges[e].second;

        int isEdgeIncluded;
        if (includeIntersected)
        {
            isEdgeIncluded = (firstNodeIndex >= 0 && m_nodeMask[firstNodeIndex] == 1 ||
                secondNodeIndex >= 0 && m_nodeMask[secondNodeIndex] == 1) ? 1 : 0;
        }
        else
        {
            isEdgeIncluded = (firstNodeIndex >= 0 && m_nodeMask[firstNodeIndex] == 1 &&
                secondNodeIndex >= 0 && m_nodeMask[secondNodeIndex] == 1) ? 1: 0;

        }

        edgeMask[e] = isEdgeIncluded;
    }

    // if one edge of the face is not included do not include all the edges of that face 
    auto secondEdgeMask = edgeMask;
    if (!includeIntersected)
    {
        for (int f = 0; f < GetNumFaces(); ++f)
        {
            bool isOneEdgeNotIncluded = false;
            for (int n = 0; n < GetNumFaceEdges(f); ++n)
            {
                auto edgeIndex = m_facesEdges[f][n];
                if (edgeIndex >= 0 && edgeMask[edgeIndex] == 0)
                {
                    isOneEdgeNotIncluded = true;
                    break;
                }
            }

            if (isOneEdgeNotIncluded)
            {
                for (int n = 0; n < GetNumFaceEdges(f); ++n)
                {
                    auto edgeIndex = m_facesEdges[f][n];
                    if (edgeIndex >= 0)
                    {
                        secondEdgeMask[edgeIndex] = 0;
                    }
                }
            }
        }
    }

    // if the selection is inverted, do not delete the edges of included faces
    if (invertMasking)
    {
        for (int e = 0; e < GetNumEdges(); ++e)
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
    return true;
}

bool GridGeom::Mesh::DeleteMesh(const Polygons& polygons, int deletionOption, bool invertDeletion)
{
    if (deletionOption == AllVerticesInside)
    {
        for (int n = 0; n < GetNumNodes(); ++n)
        {
            auto isInPolygon = polygons.IsPointInPolygon(m_nodes[n],0);
            if(invertDeletion)
            {
                isInPolygon = !isInPolygon;
            }
            if (isInPolygon)
            {
                m_nodes[n] = { doubleMissingValue,doubleMissingValue };
            }
        }
    }

    if (deletionOption == FacesWithIncludedCircumcenters)
    {
        Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);

        for (int e = 0; e < GetNumEdges(); ++e)
        {
            bool allFaceCircumcentersInPolygon = true;

            for (int f = 0; f < GetNumEdgesFaces(e); ++f)
            {
                auto faceIndex = m_edgesFaces[e][f];
                if(faceIndex<0)
                {
                    continue;
                }

                auto faceCircumcenter = m_facesCircumcenters[faceIndex];
                auto isInPolygon = polygons.IsPointInPolygon(faceCircumcenter,0);
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
            if(GetNumEdgesFaces(e)==0)
            {
                auto firstNodeIndex = m_edges[e].first;
                auto secondNodeIndex = m_edges[e].second;

                if(firstNodeIndex<0 || secondNodeIndex <0 )
                {
                    continue;
                }

                auto edgeCenter = (m_nodes[firstNodeIndex] + m_nodes[secondNodeIndex]) / 2.0;

                allFaceCircumcentersInPolygon = polygons.IsPointInPolygon(edgeCenter,0);
                if (invertDeletion)
                {
                    allFaceCircumcentersInPolygon = !allFaceCircumcentersInPolygon;
                }
            }

            if(allFaceCircumcentersInPolygon)
            {
                m_edges[e].first = -1;
                m_edges[e].second = -1;
            }
        }
    }

    if (deletionOption == FacesCompletelyIncluded)
    {
        MaskFaceEdgesInPolygon(polygons, invertDeletion, false);

        // mark the edges for deletion
        for (int e = 0; e < GetNumEdges(); ++e)
        {
            if (m_edgeMask[e] == 1)
            {
                m_edges[e].first = -1;
                m_edges[e].second = -1;
            }
        }
    }
    
    Administrate(AdministrationOptions::AdministrateMeshEdges);

    return true;
};

bool GridGeom::Mesh::MoveNode(Point newPoint, int nodeindex)
{
    Point nodeToMove = m_nodes[nodeindex];

    auto dx = GetDx(nodeToMove,newPoint, m_projection);
    auto dy = GetDy(nodeToMove,newPoint, m_projection);

    double distanceNodeToMoveFromNewPoint = std::sqrt(dx * dx + dy * dy);
    for (int n = 0; n < GetNumNodes(); ++n)
    {
        auto nodeDx = GetDx(m_nodes[n], nodeToMove, m_projection);
        auto nodeDy = GetDy(m_nodes[n], nodeToMove, m_projection);
        double distanceCurrentNodeFromNewPoint = std::sqrt(nodeDx * nodeDx + nodeDy * nodeDy);

        double factor = 0.5 * (1.0 + std::cos(std::min(distanceCurrentNodeFromNewPoint/distanceNodeToMoveFromNewPoint,1.0)* M_PI));

        m_nodes[n].x += dx * factor;
        m_nodes[n].y += dy * factor;
    }
    
    return true;
}

GridGeom::Mesh& GridGeom::Mesh::operator+=(Mesh const& rhs)
{
    if (m_projection != rhs.m_projection || rhs.GetNumNodes() == 0 || rhs.GetNumEdges() == 0)
    {
        return *this;
    }

    if (m_projection != rhs.m_projection)
    {
        return *this;
    }

    int rhsNumNodes = rhs.GetNumNodes();
    int rhsNumEdges = rhs.GetNumEdges();
    ResizeVectorIfNeeded(GetNumEdges() + rhsNumEdges, m_edges,{ doubleMissingValue, doubleMissingValue });
    ResizeVectorIfNeeded(GetNumNodes() + rhsNumNodes, m_nodes, { doubleMissingValue, doubleMissingValue });

    //copy mesh nodes
    for (int n = GetNumNodes(); n < GetNumNodes() + rhsNumNodes; ++n)
    {
        const int index = n - GetNumNodes();
        m_nodes[n] = rhs.m_nodes[index];
    }

    //copy mesh edges
    for (int e = GetNumEdges(); e < GetNumEdges() + rhsNumEdges; ++e)
    {
        const int index = e - GetNumEdges();
        m_edges[e].first = rhs.m_edges[index].first + GetNumNodes();
        m_edges[e].second = rhs.m_edges[index].second + GetNumNodes();
    }

    Administrate(AdministrationOptions::AdministrateMeshEdgesAndFaces);

    //no polygon involved, so node mask is 1 everywhere 
    m_nodeMask.resize(m_nodes.size());
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 1);

    return *this;
}

bool GridGeom::Mesh::ComputeNodeMaskFromEdgeMask()
{
    if (m_edgeMask.size() != GetNumEdges() || m_nodeMask.size() != GetNumNodes())
    {
        return true;
    }

    // fill node mask to 0
    std::fill(m_nodeMask.begin(), m_nodeMask.end(), 0);

    // compute node mask from edge mask
    for (int e = 0; e < GetNumEdges(); ++e)
    {
        if (m_edgeMask[e] != 1)
            continue;

        int firstNodeIndex = m_edges[e].first;
        int secondNodeIndex = m_edges[e].second;

        if (firstNodeIndex > 0)
        {
            m_nodeMask[firstNodeIndex] = 1;
        }
        if (secondNodeIndex > 0)
        {
            m_nodeMask[secondNodeIndex] = 1;
        }
    }

    return true;
}