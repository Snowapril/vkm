// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/base/common.h>
#import <Cocoa/Cocoa.h>

@interface VkmWindow : NSWindow
@end

@interface VkmApplication : NSObject <NSApplicationDelegate, NSWindowDelegate>

/*
* @brief Enter into application entrypoint
* @details entrypoint of application
*/
- (int)entryPoint:(int)argc argv:(char*[])argv;

/*
* @brief Destroy application
* @details 
*/
- (void) destroy;

@end