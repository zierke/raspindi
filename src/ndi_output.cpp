/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * file_output.cpp - Write output to file.
 */
#include <iostream>
#include <fstream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "ndi_output.hpp"
#include "rapidxml.hpp"

using namespace std;

NdiOutput::NdiOutput(VideoOptions const *options)
    : Output(options)
{
    this->NDI_send_create_desc.p_ndi_name = "Video Feed";
    this->pNDI_send = NDIlib_send_create(&NDI_send_create_desc);
    
    if (!pNDI_send)
    {
        std::cerr << "Failed to create NDI Send" << std::endl;
        exit(1);
    }
    // std::cout << "Width: " << options->width << " x Height: " << options->height << std::endl;
    this->NDI_video_frame.xres = options->width;
    this->NDI_video_frame.yres = options->height;
    this->NDI_video_frame.FourCC = NDIlib_FourCC_type_I420;
    this->NDI_video_frame.line_stride_in_bytes = options->width;
    
    // We are going to mark this as if it was a PTZ camera.
    NDIlib_metadata_frame_t NDI_capabilities;
    NDI_capabilities.p_data = "<ndi_capabilities ntk_ptz=\"true\" ntk_exposure_v2=\"false\"/>";
    NDIlib_send_add_connection_metadata(pNDI_send, &NDI_capabilities);

    // Create UDP server to distribute ptz commands
    this->udpsocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (this->udpsocket < 0){
        std::cerr << "Failed to create UDP socket" << std::endl;
        exit(1);
    }

    // zero out the structure

    this->dst.sin_family = AF_INET;
    this->dst.sin_addr.s_addr = inet_addr("127.0.0.1");
    this->dst.sin_port = htons(60504);
    

}

NdiOutput::~NdiOutput()
{
    close(this->udpsocket);
}

void NdiOutput::outputBuffer(void *mem, size_t size, int64_t timestamp_us, uint32_t flags)
{
    this->NDI_video_frame.p_data = (uint8_t*)mem;
    NDIlib_send_send_video_v2(this->pNDI_send, &this->NDI_video_frame);
    
    // Process any commands received from the other end of the connection.
    NDIlib_metadata_frame_t metadata_cmd;
    while (NDIlib_send_capture(pNDI_send, &metadata_cmd, 0) == NDIlib_frame_type_metadata) {
        // Parse the XML
        try {
            // Get the parser
            std::string xml(metadata_cmd.p_data);
            rapidxml::xml_document<char> parser;
            parser.parse<0>((char*)xml.data());

            // Get the tag
            rapidxml::xml_node<char>* p_node = parser.first_node();

            // Check its a node
            if ((!p_node) || (p_node->type() != rapidxml::node_element)) {
                // Not a valid message
            }

            if (!::strcasecmp(p_node->name(), "ntk_ptz_pan_tilt_speed")) {
                const rapidxml::xml_attribute<char>* p_pan_speed = p_node->first_attribute("pan_speed");
                const rapidxml::xml_attribute<char>* p_tilt_speed = p_node->first_attribute("tilt_speed");
                const float pan_speed = p_pan_speed ? (float)::atof(p_pan_speed->value()) : 0.0f;
                const float tilt_speed = p_tilt_speed ? (float)::atof(p_tilt_speed->value()) : 0.0f;

                char buffer[256];
                int n = sprintf (buffer, "PT:%0.3f:%0.3f", std::max(-1.0f, std::min(1.0f, pan_speed)), std::max(-1.0f, std::min(1.0f, tilt_speed)));
                n = sendto(this->udpsocket, buffer, n, 0, (const struct sockaddr *)&(this->dst), sizeof(struct sockaddr_in));

            } else if (!::strcasecmp(p_node->name(), "ntk_ptz_store_preset")) {
                const rapidxml::xml_attribute<char>* p_index = p_node->first_attribute("index");
                const int index = p_index ? ::atoi(p_index->value()) : 0;

                char buffer[256];
                int n = sprintf (buffer, "SP:%d", std::max(0, std::min(99, index)));
                n = sendto(this->udpsocket, buffer, n, 0, (const struct sockaddr *)&(this->dst), sizeof(struct sockaddr_in));
            
            } else if (!::strcasecmp(p_node->name(), "ntk_ptz_recall_preset")) {
                const rapidxml::xml_attribute<char>* p_index = p_node->first_attribute("index");
                const rapidxml::xml_attribute<char>* p_speed = p_node->first_attribute("speed");
                const int   index = p_index ? ::atoi(p_index->value()) : 0;
                const float speed = p_speed ? (float)::atof(p_speed->value()) : 1.0f;

                char buffer[256];
                int n = sprintf (buffer, "RP:%d:%1.2f", std::max(0, std::min(99, index)),std::max(0.0f, std::min(1.0f, speed)));
                n = sendto(this->udpsocket, buffer, n, 0, (const struct sockaddr *)&(this->dst), sizeof(struct sockaddr_in));
            }
        } catch (...) {
        }

        // Free the metadata memory
        NDIlib_send_free_metadata(pNDI_send, &metadata_cmd);
    }
}
