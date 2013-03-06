#include <iostream>

#include "util/alignedmemory.h"
#include "util/timer.h"
#include "mve/image.h"
#include "mve/imagetools.h"
#include "mve/imagefile.h"
#include "mve/trianglemesh.h"
#include "mve/meshtools.h"

#include "surf.h"
#include "sift.h"
#include "matching.h"
#include "pose.h"
#include "correspondence.h"
#include "triangulate.h"
#include "poseransac.h"
#include "visualizer.h"

int
main (int argc, char** argv)
{
   if (argc < 3)
    {
        std::cerr << "Syntax: " << argv[0] << " image1 image2" << std::endl;
        return 1;
    }

    mve::ByteImage::Ptr image1, image2;
    try
    {
        std::cout << "Loading " << argv[1] << "..." << std::endl;
        image1 = mve::image::load_file(argv[1]);
        std::cout << "Loading " << argv[2] << "..." << std::endl;
        image2 = mve::image::load_file(argv[2]);
    }
    catch (std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

#if 1
    /* Feature detection. */
    sfm::Sift::Descriptors descr1;
    {
        sfm::Sift sift;
        sift.set_min_max_octave(0, 4);
        sift.set_image(image1);
        sift.process();
        descr1 = sift.get_descriptors();
    }

    sfm::Sift::Descriptors descr2;
    {
        sfm::Sift sift;
        sift.set_min_max_octave(0, 4);
        sift.set_image(image2);
        sift.process();
        descr2 = sift.get_descriptors();
    }
#else
    sfm::Sift::Descriptors descr1;
    sfm::Sift::load_lowe_descriptors("/Users/sfuhrmann/Downloads/fountain10.descr", &descr1);
    sfm::Sift::Descriptors descr2;
    sfm::Sift::load_lowe_descriptors("/Users/sfuhrmann/Downloads/fountain20.descr", &descr2);
    std::cout << "Loaded " << descr1.size() << " and " << descr2.size() << " descriptors." << std::endl;
#endif

    /* Feature matching. */
    sfm::MatchingResult matching;
    {
        int const DIM = 128;
        /* Convert the descriptors to aligned float arrays. */
        util::AlignedMemory<float, 16> matchset_1(DIM * descr1.size());
        float* out_ptr1 = matchset_1.begin();
        for (std::size_t i = 0; i < descr1.size(); ++i, out_ptr1 += DIM)
            std::copy(descr1[i].data.begin(), descr1[i].data.end(), out_ptr1);

        util::AlignedMemory<float, 16> matchset_2(DIM * descr2.size());
        float* out_ptr2 = matchset_2.begin();
        for (std::size_t i = 0; i < descr2.size(); ++i, out_ptr2 += DIM)
            std::copy(descr2[i].data.begin(), descr2[i].data.end(), out_ptr2);

        /* Perform matching. */
        sfm::MatchingOptions matchopts;
        matchopts.descriptor_length = DIM;
        matchopts.lowe_ratio_threshold = 0.80f;
        matchopts.distance_threshold = 0.7f;

        util::WallTimer timer;
        sfm::match_features(matchopts, matchset_1.begin(), descr1.size(),
            matchset_2.begin(), descr2.size(), &matching);
        sfm::remove_inconsistent_matches(&matching);
        std::cout << "Two-view matching took " << timer.get_elapsed() << "ms, "
            << sfm::count_consistent_matches(matching) << " matches."
            << std::endl;
    }

    /* Convert matches to RANSAC data structure. */
    sfm::Correspondences matches;
    for (std::size_t i = 0; i < matching.matches_1_2.size(); ++i)
    {
        if (matching.matches_1_2[i] < 0)
            continue;

        sfm::Correspondence match;
        match.p1[0] = descr1[i].x;
        match.p1[1] = descr1[i].y;
        match.p2[0] = descr2[matching.matches_1_2[i]].x;
        match.p2[1] = descr2[matching.matches_1_2[i]].y;
        matches.push_back(match);
    }

    {
        mve::ByteImage::Ptr image, tmp_img1 = image1, tmp_img2 = image2;
        if (image1->channels() == 1)
            tmp_img1 = mve::image::expand_grayscale<unsigned char>(image1);
        if (image2->channels() == 1)
            tmp_img2 = mve::image::expand_grayscale<unsigned char>(image2);
        image = sfm::Visualizer::draw_matches(tmp_img1, tmp_img2, matches);
        mve::image::save_file(image, "/tmp/matches-initial.png");
    }

    /* Pose RANSAC. */
    sfm::PoseRansac::Result ransac_result;
    {
        sfm::PoseRansac::Options ransac_options;
        ransac_options.max_iterations = 1000;
        ransac_options.threshold = 2.0;
        ransac_options.already_normalized = false;
        sfm::PoseRansac ransac(ransac_options);

        util::WallTimer timer;
        ransac.estimate(matches, &ransac_result);
        std::cout << "RANSAC took " << timer.get_elapsed() << "ms, "
            << ransac_result.inliers.size() << " inliers." << std::endl;
    }

    /* Create new set of matches from inliners. */
    {
        sfm::Correspondences tmp_matches(ransac_result.inliers.size());
        for (std::size_t i = 0; i < ransac_result.inliers.size(); ++i)
            tmp_matches[i] = matches[ransac_result.inliers[i]];
        std::swap(matches, tmp_matches);
    }

    {
        mve::ByteImage::Ptr image, tmp_img1 = image1, tmp_img2 = image2;
        if (image1->channels() == 1)
            tmp_img1 = mve::image::expand_grayscale<unsigned char>(image1);
        if (image2->channels() == 1)
            tmp_img2 = mve::image::expand_grayscale<unsigned char>(image2);
        image = sfm::Visualizer::draw_matches(tmp_img1, tmp_img2, matches);
        mve::image::save_file(image, "/tmp/matches-ransac.png");
    }

    {
        std::cout << "Two-View matching statistics: " << std::endl;
        int const num_matches = sfm::count_consistent_matches(matching);
        int const num_inliers = ransac_result.inliers.size();
        int const num_descriptors = std::min(descr1.size(), descr2.size());
        float matching_ratio = (float)num_matches / (float)num_descriptors;
        float inlier_ratio = (float)num_inliers / (float)num_matches;
        std::cout << "  " << descr1.size() << " and " << descr2.size()
            << " descriptors" << std::endl;
        std::cout << "  " << num_matches << " matches (ratio "
            << matching_ratio << "), " <<  num_inliers << " inliers (ratio "
            << inlier_ratio << ")" << std::endl;
    }

    /* Find normalization for inliers and re-compute fundamental. */
    std::cout << "Re-computing fundamental matrix for inliers..." << std::endl;
    sfm::FundamentalMatrix F;
    {
        sfm::Correspondences tmp_matches = matches;
        math::Matrix3d T1, T2;
        sfm::compute_normalization(tmp_matches, &T1, &T2);
        sfm::apply_normalization(T1, T2, &tmp_matches);
        sfm::fundamental_least_squares(tmp_matches, &F);
        sfm::enforce_fundamental_constraints(&F);
        F = T2.transposed() * F * T1;
    }

    /* Compute pose from fundamental matrix. */
    std::cout << "Computing pose..." << std::endl;
    sfm::CameraPose pose1, pose2;
    {
        // Set K-matrices.
        int const width1 = image1->width();
        int const height1 = image1->height();
        int const width2 = image2->width();
        int const height2 = image2->height();
        double flen1 = 31.0 / 35.0 * static_cast<double>(std::max(width1, height1));
        double flen2 = 31.0 / 35.0 * static_cast<double>(std::max(width2, height2));
        pose1.set_k_matrix(flen1, width1 / 2.0, height1 / 2.0);

        pose1.init_canonical_form();
        pose2.set_k_matrix(flen2, width2 / 2.0, height2 / 2.0);
        // Compute essential from fundamental.
        sfm::EssentialMatrix E = pose2.K.transposed() * F * pose1.K;
        // Compute pose from essential.
        std::vector<sfm::CameraPose> poses;
        pose_from_essential(E, &poses);
        // Find the correct pose using point test.
        bool found_pose = false;
        for (std::size_t i = 0; i < poses.size(); ++i)
        {
            poses[i].K = pose2.K;
            if (sfm::is_consistent_pose(matches[1], pose1, poses[i]))
            {
                pose2 = poses[i];
                found_pose = true;
                break;
            }
        }
        if (!found_pose)
        {
            std::cout << "Could not find valid pose." << std::endl;
            return 1;
        }
    }

    /* Triangulate all correspondences. */
    std::cout << "Producing point model..." << std::endl;
    mve::TriangleMesh::Ptr mesh = mve::TriangleMesh::create();
    {
        for (std::size_t i = 0; i < matches.size(); ++i)
        {
            math::Vec3d x = sfm::triangulate_match(matches[i], pose1, pose2);
            mesh->get_vertices().push_back(x);
        }
    }
    mve::geom::save_mesh(mesh, "/tmp/pose.ply");

    return 0;
}
