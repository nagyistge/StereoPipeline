// __BEGIN_LICENSE__
// 
// Copyright (C) 2008 United States Government as represented by the
// Administrator of the National Aeronautics and Space Administration
// (NASA).  All Rights Reserved.
// 
// Copyright 2008 Carnegie Mellon University. All rights reserved.
// 
// This software is distributed under the NASA Open Source Agreement
// (NOSA), version 1.3.  The NOSA has been approved by the Open Source
// Initiative.  See the file COPYING at the top of the distribution
// directory tree for the complete NOSA document.
// 
// THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF ANY
// KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
// LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
// SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT
// THE SUBJECT SOFTWARE WILL BE ERROR FREE, OR ANY WARRANTY THAT
// DOCUMENTATION, IF PROVIDED, WILL CONFORM TO THE SUBJECT SOFTWARE.
//
// __END_LICENSE__

/// \file point2dem.cc
///

#ifdef _MSC_VER
#pragma warning(disable:4244)
#pragma warning(disable:4267)
#pragma warning(disable:4996)
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <stdlib.h>

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <vw/FileIO.h>
#include <vw/Image.h>
#include <vw/Math.h>
#include <vw/Stereo.h>
#include <vw/Cartography.h>
using namespace vw;
using namespace vw::stereo;
using namespace vw::cartography;

#include "stereo.h"
#include "DEM.h"
#include "nff_terrain.h"
#include "OrthoRasterizer.h"

// Apply an offset to the points in the PointImage
class PointOffsetFunc : public UnaryReturnSameType {
  Vector3 m_offset;
  
public:
  PointOffsetFunc(Vector3 const& offset) : m_offset(offset) {}    
  
  template <class T>
  T operator()(T const& p) const {
    if (p == T()) return p;
    return p + m_offset;
  }
};

template <class ImageT>
UnaryPerPixelView<ImageT, PointOffsetFunc>
inline point_image_offset( ImageViewBase<ImageT> const& image, Vector3 const& offset) {
  return UnaryPerPixelView<ImageT,PointOffsetFunc>( image.impl(), PointOffsetFunc(offset) );
}


// Allows FileIO to correctly read/write these pixel types
namespace vw {
  template<> struct PixelFormatID<Vector3>   { static const PixelFormatEnum value = VW_PIXEL_GENERIC_3_CHANNEL; };
}

class PointTransFunc : public ReturnFixedType<Vector3> {
  Matrix3x3 m_trans;
public:
  PointTransFunc(Matrix3x3 trans) : m_trans(trans) {}
  Vector3 operator() (Vector3 const& pt) const { return m_trans*pt; }
};


int main( int argc, char *argv[] ) {
  set_debug_level(VerboseDebugMessage+11);

  std::string input_file_name, out_prefix, output_file_type, texture_filename;
  unsigned cache_size;
  float dem_spacing, default_value=0;
  int debug_level;
  double semi_major, semi_minor;
  std::string reference_spheroid;
  double phi_rot, omega_rot, kappa_rot;
  std::string rot_order;
  double proj_lat=0, proj_lon=0, proj_scale=1;
  double z_offset;
  unsigned utm_zone;

  po::options_description desc("Options");
  desc.add_options()
    ("help", "Display this help message")
    ("default-value", po::value<float>(&default_value), "Explicitly set the default (missing pixel) value.  By default, the min z value is used.")
    ("use-alpha", "Create images that have an alpha channel")
    ("dem-spacing,s", po::value<float>(&dem_spacing)->default_value(0), "Set the DEM post size (if this value is 0, the post spacing size is computed for you)")
    ("normalized,n", "Also write a normalized version of the DEM (for debugging)")    
    ("orthoimage", po::value<std::string>(&texture_filename), "Write an orthoimage based on the texture file given as an argument to this command line option")
    ("grayscale", "Use grayscale image processing for creating the orthoimage")
    ("offset-files", "Also write a pair of ascii offset files (for debugging)")    
    ("cache", po::value<unsigned>(&cache_size)->default_value(2048), "Cache size, in megabytes")
    ("input-file", po::value<std::string>(&input_file_name), "Explicitly specify the input file")
    ("texture-file", po::value<std::string>(&texture_filename), "Specify texture filename")
    ("output-prefix,o", po::value<std::string>(&out_prefix)->default_value("terrain"), "Specify the output prefix")
    ("output-filetype,t", po::value<std::string>(&output_file_type)->default_value("tif"), "Specify the output file")
    ("debug-level,d", po::value<int>(&debug_level)->default_value(vw::DebugMessage-1), "Set the debugging output level. (0-50+)")
    ("xyz-to-lonlat", "Convert from xyz coordinates to longitude, latitude, altitude coordinates.")
    ("reference-spheroid,r", po::value<std::string>(&reference_spheroid)->default_value(""),"Set a reference surface to a hard coded value (one of [moon , mars].  This will override manually set datum information.")
    ("semi-major-axis", po::value<double>(&semi_major)->default_value(0),"Set the dimensions of the datum.")
    ("semi-minor-axis", po::value<double>(&semi_minor)->default_value(0),"Set the dimensions of the datum.")
    ("z-offset", po::value<double>(&z_offset)->default_value(0), "Add a vertical offset to the DEM")

    ("sinusoidal", "Save using a sinusoidal projection")
    ("mercator", "Save using a Mercator projection")
    ("transverse-mercator", "Save using a transverse Mercator projection")
    ("orthographic", "Save using an orthographic projection")
    ("stereographic", "Save using a stereographic projection")
    ("lambert-azimuthal", "Save using a Lambert azimuthal projection")
    ("utm", po::value<unsigned>(&utm_zone), "Save using a UTM projection with the given zone")
    ("proj-lat", po::value<double>(&proj_lat), "The center of projection latitude (if applicable)")
    ("proj-lon", po::value<double>(&proj_lon), "The center of projection longitude (if applicable)")
    ("proj-scale", po::value<double>(&proj_scale), "The projection scale (if applicable)")

    ("rotation-order", po::value<std::string>(&rot_order)->default_value("xyz"),"Set the order of an euler angle rotation applied to the 3D points prior to DEM rasterization")
    ("phi-rotation", po::value<double>(&phi_rot)->default_value(0),"Set a rotation angle phi")
    ("omega-rotation", po::value<double>(&omega_rot)->default_value(0),"Set a rotation angle omega")
    ("kappa-rotation", po::value<double>(&kappa_rot)->default_value(0),"Set a rotation angle kappa");
  
  po::positional_options_description p;
  p.add("input-file", 1);

  po::variables_map vm;
  po::store( po::command_line_parser( argc, argv ).options(desc).positional(p).run(), vm );
  po::notify( vm );

  // Set the Vision Workbench debug level
  set_debug_level(debug_level);
  Cache::system_cache().resize( cache_size*1024*1024 ); 

  if( vm.count("help") ) {
    std::cout << desc << std::endl;
    return 1;
  }

  if( vm.count("input-file") != 1 ) {
    std::cout << "Error: Must specify exactly one pointcloud file and one texture file!" << std::endl;
    std::cout << desc << std::endl;
    return 1;
  }

  DiskImageView<Vector3> point_disk_image(input_file_name);
  ImageViewRef<Vector3> point_image = point_disk_image;

  // Apply an (optional) rotation to the 3D points before building the mesh.
  if (phi_rot != 0 || omega_rot != 0 || kappa_rot != 0) {
    std::cout << "\t--> Applying rotation sequence: " << rot_order << "      Angles: " << phi_rot << "   " << omega_rot << "  " << kappa_rot << "\n";
    Matrix3x3 rotation_trans = math::euler_to_rotation_matrix(phi_rot,omega_rot,kappa_rot,rot_order);
    point_image = per_pixel_filter(point_image, PointTransFunc(rotation_trans));
  }

  if (vm.count("xyz-to-lonlat") ) {
    std::cout << "\t--> Reprojecting points into longitude, latitude, altitude.\n";
    point_image = cartography::xyz_to_lon_lat_radius(point_image);    
  }

  // Select a cartographic DATUM.  There are several hard coded datums
  // that can be used here, or the user can specify their own.
  cartography::Datum datum;
  if ( reference_spheroid != "" ) {
    if (reference_spheroid == "mars") {
      const double MOLA_PEDR_EQUATORIAL_RADIUS = 3396000.0;
      std::cout << "\t--> Re-referencing altitude values using standard MOLA spherical radius: " << MOLA_PEDR_EQUATORIAL_RADIUS << "\n";
      datum = cartography::Datum("D_MARS",
                                 "MARS",
                                 "Reference Meridian",
                                 MOLA_PEDR_EQUATORIAL_RADIUS,
                                 MOLA_PEDR_EQUATORIAL_RADIUS,
                                 0.0);
    } else if (reference_spheroid == "moon") {
      const double LUNAR_RADIUS = 1737400;
      std::cout << "\t--> Re-referencing altitude values using standard lunar spherical radius: " << LUNAR_RADIUS << "\n";
      datum = cartography::Datum("D_MOON",
                                 "MOON",
                                 "Reference Meridian",
                                 LUNAR_RADIUS,
                                 LUNAR_RADIUS,
                                 0.0);
    } else {
      std::cout << "\t--> Unknown reference spheroid: " << reference_spheroid << ".  Current options are [ moon , mars ]\nExiting.\n\n";
      exit(0);
    }
  } else if (semi_major != 0 && semi_minor != 0) {
    std::cout << "\t--> Re-referencing altitude values to user supplied datum.  Semi-major: " << semi_major << "  Semi-minor: " << semi_minor << "\n";
    datum = cartography::Datum("User Specified Datum",
                               "User Specified Spheroid",
                               "Reference Meridian",
                               semi_major, semi_minor, 0.0);
  }

  if (z_offset != 0) {
    std::cout << "\t--> Applying z-offset: " << z_offset << "\n";
    point_image = point_image_offset(point_image, Vector3(0,0,z_offset));
  }

  // Set up the georeferencing information.  We specify everything
  // here except for the affine transform, which is defined later once
  // we know the bounds of the orthorasterizer view.  However, we can
  // still reproject the points in the point image without the affine
  // transform because this projection never requires us to convert to
  // or from pixel space.
  GeoReference georef(datum);

  // If the data was left in cartesian coordinates, we need to give
  // the DEM a projection that uses some physical units (meters),
  // rather than lon, lat.  This is actually mainly for compatibility
  // with Viz, and it's sort of a hack, but it's left in for the time
  // being.
  //
  // Otherwise, we honor the user's requested projection and convert
  // the points if necessary.
  if (!vm.count("xyz-to-lonlat"))
    georef.set_mercator(0,0,1);
  else if( vm.count("sinusoidal") ) georef.set_sinusoidal(proj_lon);
  else if( vm.count("mercator") ) georef.set_mercator(proj_lat,proj_lon,proj_scale);
  else if( vm.count("transverse-mercator") ) georef.set_transverse_mercator(proj_lat,proj_lon,proj_scale);
  else if( vm.count("orthographic") ) georef.set_orthographic(proj_lat,proj_lon);
  else if( vm.count("stereographic") ) georef.set_stereographic(proj_lat,proj_lon,proj_scale);
  else if( vm.count("lambert-azimuthal") ) georef.set_lambert_azimuthal(proj_lat,proj_lon);
  else if( vm.count("utm") ) georef.set_UTM( utm_zone );

  if (vm.count("xyz-to-lonlat"))
    point_image = cartography::project_point_image(point_image, georef);

  // Rasterize the results to a temporary file on disk so as to speed
  // up processing in the orthorasterizer, which accesses each pixel
  // multiple times.
  DiskCacheImageView<Vector3> point_image_cache(point_image, "tif");
  
  // write out the DEM, texture, and extrapolation mask as
  // georeferenced files.
  vw::cartography::OrthoRasterizerView<PixelGray<float> > rasterizer(point_image_cache, select_channel(point_image_cache,2), dem_spacing);
  if (!vm.count("default-value") ) {
    rasterizer.set_use_minz_as_default(true); 
  } else {
    rasterizer.set_use_minz_as_default(false); 
    rasterizer.set_default_value(default_value);    
  }

  if (vm.count("use-alpha")) {
    rasterizer.set_use_alpha(true);
  }

  vw::BBox<float,3> dem_bbox = rasterizer.bounding_box();
  std::cout << "\nDEM Bounding box: " << dem_bbox << "\n";
  
  // Now we are ready to specify the affine transform.
  Matrix3x3 georef_affine_transform = rasterizer.geo_transform();
  std::cout << "Georeferencing Transform: " << georef_affine_transform << "\n";
  georef.set_transform(georef_affine_transform);

  // Write out a georeferenced orthoimage of the DTM with alpha.
  if (vm.count("orthoimage")) {
    rasterizer.set_use_minz_as_default(false);
    DiskImageView<PixelGray<float> > texture(texture_filename); 
    rasterizer.set_texture(texture);
    BlockCacheView<PixelGray<float> > block_drg_raster(rasterizer, Vector2i(rasterizer.cols(), 2048));
    write_georeferenced_image(out_prefix + "-DRG.tif", channel_cast_rescale<uint8>(block_drg_raster), georef, TerminalProgressCallback() );

  } else {
    // Write out the DEM.
    std::cout << "\nWriting DEM.\n";
    BlockCacheView<PixelGray<float> > block_dem_raster(rasterizer, Vector2i(rasterizer.cols(), 2024));
    write_georeferenced_image(out_prefix + "-DEM." + output_file_type, block_dem_raster, georef, TerminalProgressCallback());

    // Write out a normalized version of the DTM (for debugging)
    if (vm.count("normalized")) {
      std::cout << "\nWriting normalized DEM.\n";
      DiskImageView<PixelGray<float> > dem_image(out_prefix + "-DEM." + output_file_type);
      write_georeferenced_image(out_prefix + "-DEM-normalized.tif", channel_cast_rescale<uint8>(normalize(dem_image)), georef, TerminalProgressCallback());
    }
  }

  // Write out the offset files
  if (vm.count("offset-files")) {
    std::cout << "Offset: " << dem_bbox.min().x()/rasterizer.spacing() << "   " << dem_bbox.max().y()/rasterizer.spacing() << "\n";
    std::string offset_filename = out_prefix + "-DRG.offset";
    FILE* offset_file = fopen(offset_filename.c_str(), "w");
    fprintf(offset_file, "%d\n%d\n", int(dem_bbox.min().x()/rasterizer.spacing()), -int(dem_bbox.max().y()/rasterizer.spacing()));
    fclose(offset_file);
    offset_filename = out_prefix + "-DEM-normalized.offset";
    offset_file = fopen(offset_filename.c_str(), "w");
    fprintf(offset_file, "%d\n%d\n", int(dem_bbox.min().x()/rasterizer.spacing()), -int(dem_bbox.max().y()/rasterizer.spacing()));
    fclose(offset_file);
  }
  return 0;
}
