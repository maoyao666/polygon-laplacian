//=============================================================================

#include "PolyDiffGeo.h"
#include <pmp/algorithms/DifferentialGeometry.h>
#include <pmp/algorithms/SurfaceTriangulation.h>

//=============================================================================

using namespace std;
using namespace pmp;

using SparseMatrix = Eigen::SparseMatrix<double>;
using Triplet = Eigen::Triplet<double>;

//=============================================================================

const double eps = 1e-10;
bool clamp_cotan_ = false;

//=============================================================================

void setup_prolongation_matrix(SurfaceMesh &mesh, SparseMatrix &A) 
{
    auto area_weights = mesh.get_face_property<Eigen::VectorXd>("f:weights");

    const unsigned int nv = mesh.n_vertices();
    const unsigned int nf = mesh.n_faces();
    Eigen::VectorXd w;

    std::vector<Triplet> tripletsA;
    Vertex v;
    Face f;
    for (auto v : mesh.vertices()) {
        tripletsA.emplace_back(v.idx(), v.idx(), 1.0);
    }

    unsigned int j = 0;
    for (auto f : mesh.faces()) {
        w = area_weights[f];
        unsigned int i = 0;
        for (auto v : mesh.vertices(f)) {
            tripletsA.emplace_back(nv + j, v.idx(), w(i));
            i++;
        }
        j++;
    }

    // build sparse matrix from triplets
    A.resize(nv + nf, nv);
    A.setFromTriplets(tripletsA.begin(), tripletsA.end());
}

//-----------------------------------------------------------------------------

void lump_matrix(SparseMatrix &D) {

    std::vector<Triplet> triplets;
    triplets.reserve(D.rows() * 6);
    for (int k = 0; k < D.outerSize(); ++k) {
        for (SparseMatrix::InnerIterator it(D, k); it; ++it) {
            triplets.emplace_back(it.row(), it.row(), it.value());
        }
    }
    D.setFromTriplets(triplets.begin(), triplets.end());
}

//===================Area Computations ==========================================================

Scalar polygon_surface_area(const SurfaceMesh &mesh) {
    Scalar area(0);
    for (auto f : mesh.faces()) {
        area += face_area(mesh, f);
    }
    return area;
}

//-----------------------------------------------------------------------------

double face_area(const SurfaceMesh &mesh, Face f) {

    double a = 0.0;
    Point C = centroid(mesh, f);
    Point Q, R;
    for (auto h : mesh.halfedges(f)) {
        // three vertex positions
        Q = mesh.position(mesh.from_vertex(h));
        R = mesh.position(mesh.to_vertex(h));

        a += pmp::triangle_area(C, Q, R);
    }

    return a;
}

//-----------------------------------------------------------------------------

Point area_weighted_centroid(const SurfaceMesh &mesh) {
    Point center(0, 0, 0), c;
    Scalar area(0), a;
    for (auto f : mesh.faces()) {
        int count = 0;
        c = Point(0, 0, 0);
        for (auto v : mesh.vertices(f)) {
            c += mesh.position(v);
            count++;
        }
        c /= (Scalar) count;
        a = (Scalar) face_area(mesh, f);
        area += a;
        center += a * c;
    }
    return center /= area;

}

//===================Gradient matrix Computation ==========================================================

Eigen::Vector3d gradient_hat_function(Point i, Point j, Point k) {
    Point base, site, grad;
    Eigen::Vector3d gradient;
    double area;
    area = triangle_area(i, j, k);
    site = i - j;
    base = k - j;
    grad = site - (dot(site, base) / norm(base)) * base / norm(base);
    if (area < eps) {
        gradient = Eigen::Vector3d(0, 0, 0);
    } else {
        grad = norm(base) * grad / norm(grad);
        gradient = Eigen::Vector3d(grad[0], grad[1], grad[2]) / (2.0 * area);
    }

    return gradient;
}

//-----------------------------------------------------------------------------

void setup_Gradient_Matrix(SurfaceMesh &mesh, SparseMatrix &G) {

    SparseMatrix A;
    setup_prolongation_matrix(mesh, A);

    const unsigned int nv = mesh.n_vertices();
    const unsigned int nf = mesh.n_faces();
    Point p, p0, p1;
    Vertex v0, v1;
    int nr_triangles = 0;
    int k = 0;
    auto area_points = mesh.get_face_property<Point>("f:point");
    Eigen::Vector3d gradient_p, gradient_p0, gradient_p1;
    // nonzero elements of G as triplets: (row, column, value)
    std::vector<Triplet> triplets;

    for (Face f : mesh.faces()) {
        nr_triangles += mesh.valence(f);
        p = area_points[f];
        for (auto h : mesh.halfedges(f)) {
            v0 = mesh.from_vertex(h);
            v1 = mesh.to_vertex(h);

            p0 = mesh.position(v0);
            p1 = mesh.position(v1);

            gradient_p = gradient_hat_function(p, p0, p1);
            gradient_p0 = gradient_hat_function(p0, p1, p);
            gradient_p1 = gradient_hat_function(p1, p, p0);

            for (int j = 0; j < 3; j++) {
                triplets.emplace_back(3 * k + j, nv + f.idx(), gradient_p(j));
                triplets.emplace_back(3 * k + j, v0.idx(), gradient_p0(j));
                triplets.emplace_back(3 * k + j, v1.idx(), gradient_p1(j));
            }
            k++;
        }
    }

    G.resize(3 * nr_triangles, nv + nf);
    G.setFromTriplets(triplets.begin(), triplets.end());
    G = G * A;
}

//-----------------------------------------------------------------------------

void setup_Divergence_Matrix(SurfaceMesh &mesh, SparseMatrix &Gt) {
    SparseMatrix G, M;
    setup_Gradient_Matrix(mesh, G);
    setup_Gradient_Mass_Matrix(mesh, M);
    Gt = -G.transpose() * M;
}
//-----------------------------------------------------------------------------

void setup_Gradient_Mass_Matrix(SurfaceMesh &mesh,
                                Eigen::SparseMatrix<double> &M) {
    auto area_points = mesh.get_face_property<Point>("f:point");
    double area;
    std::vector<Eigen::Triplet<double>> triplets;
    int valence, idx, c = 0;
    for (auto f : mesh.faces()) {
        valence = mesh.valence(f);
        int i = 0;
        for (auto h : mesh.halfedges(f)) {
            Point p0 = mesh.position(mesh.from_vertex(h));
            Point p1 = mesh.position(mesh.to_vertex(h));
            area = triangle_area(p0, p1, area_points[f]);
            for (int j = 0; j < 3; j++) {
                idx = c + 3 * i + j;
                triplets.emplace_back(idx, idx, area);
            }
            i++;
        }
        c += valence * 3;
    }
    M.resize(c, c);

    M.setFromTriplets(triplets.begin(), triplets.end());
}

//===================Minimization for Squared Area Point through vertex weights derivatives=============================

void setup_face_point_properties(SurfaceMesh &mesh) {

    auto area_points = mesh.get_face_property<Point>("f:point");
    auto area_weights = mesh.get_face_property<Eigen::VectorXd>("f:weights");

    Eigen::MatrixXd T, P, PP;
    std::vector<Eigen::VectorXd> weights, testWeights, testWeightsPhil;
    Eigen::VectorXd w;
    Eigen::MatrixXd poly;

    std::vector<Eigen::Triplet<double>> trip;

    for (Face f: mesh.faces()) {
        const int n = mesh.valence(f);
        poly.resize(n, 3);
        int i = 0;
        for (Vertex v : mesh.vertices(f)) {
            for (int h = 0; h < 3; h++) {
                poly.row(i)(h) = mesh.position(v)[h];
            }
            i++;
        }
        find_polygon_weights(poly, w);
        Eigen::Vector3d min = poly.transpose() * w;
        area_points[f] = Point(min(0), min(1), min(2));
        area_weights[f] = w;
    }
}
// compute minimizing points per face (P) and their konvex combination weights (weights)



//--------------!!!!!!!!!!!!!!!!!!!!! IMPORTANT !!!!!!!!!!!!!!!!!!!!!!!!!!!!!------------------------------------------------------------------

void find_polygon_weights(const Eigen::MatrixXd &poly,
                          Eigen::VectorXd &weights) {
    int val = poly.rows();
    Eigen::MatrixXd J(val, val);
    Eigen::VectorXd b(val);
    weights.resize(val);

    for (int i = 0; i < val; i++) {
        Eigen::Vector3d pk = poly.row(i);

        double Bk1_d2 = 0.0;
        double Bk1_d1 = 0.0;

        double Bk2_d0 = 0.0;
        double Bk2_d2 = 0.0;

        double Bk3_d0 = 0.0;
        double Bk3_d1 = 0.0;

        double CBk = 0.0;
        Eigen::Vector3d d = Eigen::MatrixXd::Zero(3, 1);

        for (int j = 0; j < val; j++) {
            Eigen::Vector3d pi = poly.row(j);
            Eigen::Vector3d pj = poly.row((j + 1) % val);
            d = pi - pj;


            double Bik1 = d(1) * pk(2) - d(2) * pk(1);
            double Bik2 = d(2) * pk(0) - d(0) * pk(2);
            double Bik3 = d(0) * pk(1) - d(1) * pk(0);

            double Ci1 = d(1) * pi(2) - d(2) * pi(1);
            double Ci2 = d(2) * pi(0) - d(0) * pi(2);
            double Ci3 = d(0) * pi(1) - d(1) * pi(0);

            Bk1_d1 += d(1) * Bik1;
            Bk1_d2 += d(2) * Bik1;

            Bk2_d0 += d(0) * Bik2;
            Bk2_d2 += d(2) * Bik2;

            Bk3_d0 += d(0) * Bik3;
            Bk3_d1 += d(1) * Bik3;

            CBk += Ci1 * Bik1 + Ci2 * Bik2 + Ci3 * Bik3;
        }
        for (int k = 0; k < val; k++) {
            Eigen::Vector3d xj = poly.row(k);
            J(i, k) = 0.5 * (xj(2) * Bk1_d1 - xj(1) * Bk1_d2 + xj(0) * Bk2_d2 -
                             xj(2) * Bk2_d0 + xj(1) * Bk3_d0 - xj(0) * Bk3_d1);
        }
        b(i) = 0.5 * CBk;

    }

    Eigen::MatrixXd M(val+1, val);
    M.block(0, 0, val, val) = 4*J;
    M.block(val, 0, 1, val).setOnes();


    Eigen::VectorXd b_(val + 1);
    b_.block(0, 0, val, 1) = 4*b;
//    b_.block(0, 0, val, 1) = b;

    b_(val) = 1.;

    weights = M.completeOrthogonalDecomposition().solve(b_).topRows(val);
}

//----------------------------------------------------------------------------------

void setup_stiffness_matrix(SurfaceMesh &mesh, Eigen::SparseMatrix<double> &S) {
    const int nv = mesh.n_vertices();

    Eigen::MatrixXd Si;
    Eigen::Vector3d min;
    Eigen::VectorXd w;
    Eigen::MatrixXd poly;

    std::vector<Eigen::Triplet<double>> trip;

    for (Face f: mesh.faces()) {
        const int n = mesh.valence(f);
        poly.resize(n, 3);
        int i = 0;
        for (Vertex v : mesh.vertices(f)) {
            for (int h = 0; h < 3; h++) {
                poly.row(i)(h) = mesh.position(v)[h];
            }
            i++;
        }

        // compute weights for the polygon
        find_polygon_weights(poly, w);

        Eigen::Vector3d min = poly.transpose() * w;
        localStiffnessMatrix(poly, min, w, Si);

        int j = 0;
        int k;
        for (Vertex v : mesh.vertices(f)) {
            k = 0;
            for (Vertex vv : mesh.vertices(f)) {
                trip.emplace_back(vv.idx(), v.idx(), Si(k, j));
                k++;
            }
            j++;
        }
    }
    S.resize(nv, nv);
    S.setFromTriplets(trip.begin(), trip.end());
    S *= -1.0;

}

//----------------------------------------------------------------------------------


void localStiffnessMatrix(const Eigen::MatrixXd &poly, const Eigen::Vector3d &min, Eigen::VectorXd &w,
                          Eigen::MatrixXd &L) {
    const int n = (int) poly.rows();
    L.resize(n, n);
    L.setZero();

    Eigen::VectorXd ln(n + 1);
    ln.setZero();

    double l[3], l2[3];

    for (int i = 0; i < n; ++i) {
        const int i1 = (i + 1) % n;

        l2[2] = (poly.row(i) - poly.row(i1)).squaredNorm();
        l2[0] = (poly.row(i1) - min.transpose()).squaredNorm();
        l2[1] = (poly.row(i) - min.transpose()).squaredNorm();

        l[0] = sqrt(l2[0]);
        l[1] = sqrt(l2[1]);
        l[2] = sqrt(l2[2]);

        const double arg =
                (l[0] + (l[1] + l[2])) * (l[2] - (l[0] - l[1])) * (l[2] + (l[0] - l[1])) * (l[0] + (l[1] - l[2]));
        const double area = 0.5 * sqrt(arg);
        if (area >  1e-7 ) {
            l[0] = 0.25 * (l2[1] + l2[2] - l2[0]) / area;
            l[1] = 0.25 * (l2[2] + l2[0] - l2[1]) / area;
            l[2] = 0.25 * (l2[0] + l2[1] - l2[2]) / area;

            L(i1, i1) += l[0];
            L(i, i) += l[1];
            L(i1, i) -= l[2];
            L(i, i1) -= l[2];
            L(i, i) += l[2];
            L(i1, i1) += l[2];

            ln(i1) -= l[0];
            ln(i) -= l[1];
            ln(n) += l[0] + l[1];
        }
    }

    // Sandwiching
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            L(i, j) += w(i) * ln(j) + w(j) * ln(i) + w(i) * w(j) * ln(n);
}

//----------------------------------------------------------------------------------

void localMassMatrix(const Eigen::MatrixXd &poly, const Eigen::Vector3d &min, Eigen::VectorXd &w,
                     Eigen::MatrixXd &M) {
    const int n = (int) poly.rows();
    M.resize(n, n);

    M.setZero();

    Eigen::VectorXd ln(n + 1);
    ln.setZero();

    double l[3], l2[3];

    for (int i = 0; i < n; ++i) {
        const int i1 = (i + 1) % n;

        l2[2] = (poly.row(i) - poly.row(i1)).squaredNorm();
        l2[0] = (poly.row(i1) - min.transpose()).squaredNorm();
        l2[1] = (poly.row(i) - min.transpose()).squaredNorm();

        l[0] = sqrt(l2[0]);
        l[1] = sqrt(l2[1]);
        l[2] = sqrt(l2[2]);

        const double arg =
                (l[0] + (l[1] + l[2])) * (l[2] - (l[0] - l[1])) * (l[2] + (l[0] - l[1])) * (l[0] + (l[1] - l[2]));
        const double area = 0.25 * sqrt(arg);


        l[0] = 1.0 / 6.0 * area;
        l[1] = 1.0 / 12.0 * area;


        M(i1, i1) += 1.0 / 6.0 * area;
        M(i, i) += 1.0 / 6.0 * area;
        M(i1, i) += 1.0 / 12.0 * area;
        M(i, i1) += 1.0 / 12.0 * area;

        ln(i1) += l[1];
        ln(i) += l[1];
        ln(n) += l[0];
    }
    // Sandwiching
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            M(i, j) += w(i) * ln(j) + w(j) * ln(i) + w(i) * w(j) * ln(n);

}


//----------------------------------------------------------------------------------

void setup_mass_matrix(SurfaceMesh &mesh, Eigen::SparseMatrix<double> &M, bool lumped) {


    const int nv = mesh.n_vertices();

    Eigen::MatrixXd Mi;
    Eigen::Vector3d min;
    Eigen::VectorXd w;
    Eigen::MatrixXd poly;

    std::vector<Eigen::Triplet<double>> trip;

    for (Face f: mesh.faces()) {
        const int n = mesh.valence(f);
        poly.resize(n, 3);
        int i = 0;
        for (Vertex v : mesh.vertices(f)) {
            for (int h = 0; h < 3; h++) {
                poly.row(i)(h) = mesh.position(v)[h];
            }
            i++;
        }

        // setup polygon weights
        find_polygon_weights(poly, w);

        Eigen::Vector3d min = poly.transpose() * w;
        localMassMatrix(poly, min, w, Mi);

        int j = 0;
        int k;
        for (Vertex v : mesh.vertices(f)) {
            k = 0;
            for (Vertex vv : mesh.vertices(f)) {
                trip.emplace_back(vv.idx(), v.idx(), Mi(k, j));
                k++;
            }
            j++;
        }
    }
    M.resize(nv, nv);
    M.setFromTriplets(trip.begin(), trip.end());


    if (lumped) {
        lump_matrix(M);
    }
}

