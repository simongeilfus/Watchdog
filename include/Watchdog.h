/*
 
 Watchdog
 
 Copyright (c) 2014, Simon Geilfus
 All rights reserved.
 
 Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 the following conditions are met:
 
 * Redistributions of source code must retain the above copyright notice, this list of conditions and
 the following disclaimer.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
 */

/*
 Watchdog is a tiny header only library that allows to watch files or directories.
 It is supposed to be compatible with any application that supports c++11 and uses boost and/or cinder.
 
 **Watchdog is currently a work-in-progress.**
 
 By default watchdog is disabled in release mode and will only execute the provided callback once when 
 wd::watch is called and do nothing for the other methods. Undef WATCHDOG_ONLY_IN_DEBUG if you want 
 Watchdog to work in release mode.
 
 **For the moment non-Cinder application will have their callbacks called in a separated thread!**
 
 The API is very small an only expose those 3 functions to watch or unwatch files or directories:
 
 ``` c++
 wd::watch( const fs::path &path, const std::function<void(const fs::path&)> &callback )
 wd::unwatch( const fs::path &path )
 wd::unwatchAll()
 ```
 
 You can use a wildcard character to only watch for the desired files in a directory :
 
 ``` c++
 wd::watch( getAssetPath( "" ) / "shaders/lighting.*", []( const fs::path &path ){
	// do something
 } );
 ```
 
 If Watchdog is used in a Cinder context, the two functions watchAsset/unwatchAsset are available as 
 shortcuts, making the previous example shorter:
 
 ``` c++
 wd::watchAsset( "shaders/lighting.*", []( const fs::path &path ){
	// do something
 } );
 ```
*/

#pragma once

#include <map>
#include <string>
#include <thread>
#include <ctime>
#include <memory>
#include <atomic>

#ifdef CINDER_CINDER
    #include "cinder/Filesystem.h"
#else
    #if defined( CINDER_WINRT )
        #include <filesystem>
        namespace ci { namespace fs = std::tr2::sys; }
    #else
        #define BOOST_FILESYSTEM_VERSION 3
        #define BOOST_FILESYSTEM_NO_DEPRECATED
        #include <boost/filesystem.hpp>
        namespace ci { namespace fs = boost::filesystem; }
    #endif
#endif

// By default watchdog is disabled in release mode and will only execute the
// provided callback once when wd::watch is called and do nothing for the
// other methods. Undef this if you want Watchdog to work in release mode.
#define WATCHDOG_ONLY_IN_DEBUG

//! Exception for when Watchdog can't locate a file or parse the wildcard
class WatchedFileSystemExc : public ci::Exception {
public:
    WatchedFileSystemExc( const ci::fs::path &path )
    {
        sprintf( mMessage, "Failed to find file or directory at: %s", path.c_str() );
    }
    
    virtual const char * what() const throw() { return mMessage; }
    
    char mMessage[4096];
};

//! The main class
class Watchdog {
public:
    
    //! watches a file or directory for modification and call back the specified std::function
    static void watch( const ci::fs::path &path, const std::function<void(const ci::fs::path&)> &callback )
    {
        static Watchdog fs;
        fs.watchImpl( path, callback );
    }
    //! unwatches a previously registrated file or directory
    static void unwatch( const ci::fs::path &path )
    {
        watch( path, std::function<void(const ci::fs::path&)>() );
    }
    
    //! unwatches all previously registrated file or directory
    static void unwatchAll()
    {
        watch( ci::fs::path(), std::function<void(const ci::fs::path&)>() );
    }
    
#ifdef CINDER_CINDER
    //! watches an asset for modification and call back the specified std::function
    static void watchAsset( const ci::fs::path &assetPath, const std::function<void(const ci::fs::path&)> &callback )
    {
        static Watchdog fs;
        fs.watchImpl( ci::app::getAssetPath( "" ) / assetPath, callback );
    }
    //! unwatches a previously registrated asset
    static void unwatchAsset( const ci::fs::path &assetPath )
    {
        watch( ci::app::getAssetPath( "" ) / assetPath, std::function<void(const ci::fs::path&)>() );
    }
#endif
    
protected:
    
    ~Watchdog()
    {
        // remove all watchers when the static Watchdog is deleted
        unwatchAll();
    }
    
    
    class Watcher {
    public:
        Watcher( const ci::fs::path &path, const std::string &filter, const std::function<void(const ci::fs::path&)> &callback )
        : mWatching(true),mPath(path), mFilter(filter), mCallback(callback)
        {
        }
        
        ~Watcher()
        {
            mWatching = false;
            if( mThread->joinable() ) mThread->join();
        }
        
        void start()
        {
            // make sure we store all initial write time before starting the thread
            if( !mFilter.empty() ) {
                size_t wildcardPos  = mFilter.find( "*" );
                if( wildcardPos != std::string::npos ) {
                    std::string before  = mFilter.substr( 0, wildcardPos );
                    std::string after   = mFilter.substr( wildcardPos + 1 );
                    ci::fs::directory_iterator end;
                    for( ci::fs::directory_iterator it( mPath ); it != end; ++it ){
                        std::string p       = it->path().string();
                        size_t beforePos    = p.find( before );
                        size_t afterPos     = p.find_last_of( after );
                        if( ( beforePos != std::string::npos || before.empty() )
                           && ( afterPos != std::string::npos || after.empty() ) ) {
                            hasChanged( it->path() );
                        }
                    }
                }
                // this means that the first watch won't call the callback function
                // so we have to manually call it here
                mCallback( mPath / mFilter );
            }
            
            mThread = std::unique_ptr<std::thread>( new std::thread( &Watcher::watch, this ) );
        }
        
        void watch()
        {
            // keep watching for modifications every ms milliseconds
            auto ms = std::chrono::milliseconds( 500 );
            while( mWatching ){
                
                // check if the file or parent directory has changed
                if( hasChanged( mPath ) ){
                    
                    // if there's no filter we just check one item
                    if( mFilter.empty() && mCallback ){
#ifdef CINDER_CINDER
                        ci::app::App::get()->dispatchAsync( [this](){
                            mCallback( mPath );
                        } );
#else
                        mCallback( mPath );
                        //#error TODO: still have to figure out an elegant way to do this without cinder
#endif
                        
                    }
                    // otherwise we check the whole parent directory
                    else {
                        size_t wildcardPos  = mFilter.find( "*" );
                        if( wildcardPos != std::string::npos ) {
                            std::string before  = mFilter.substr( 0, wildcardPos );
                            std::string after   = mFilter.substr( wildcardPos + 1 );
                            ci::fs::directory_iterator end;
                            for( ci::fs::directory_iterator it( mPath ); it != end; ++it ){
                                std::string p       = it->path().string();
                                size_t beforePos    = p.find( before );
                                size_t afterPos     = p.find_last_of( after );
                                if( ( beforePos != std::string::npos || before.empty() )
                                   && ( afterPos != std::string::npos || after.empty() ) ) {
                                    if( hasChanged( it->path() ) && mCallback ){
#ifdef CINDER_CINDER
                                        ci::app::App::get()->dispatchAsync( [it,this](){
                                            mCallback( mPath / mFilter );
                                        } );
#else
                                        mCallback( mPath / mFilter );
                                        //#error TODO: still have to figure out an elegant way to do this without cinder
#endif
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                // make this thread sleep for a while
                std::this_thread::sleep_for( ms );
            }
        }
        
        bool hasChanged( const ci::fs::path &path ) {
            // get the last modification time
            std::time_t time = ci::fs::last_write_time( path );
            // add a new modification time to the map
            std::string key = path.string();
            if( mModificationTimes.find( key ) == mModificationTimes.end() ) {
                mModificationTimes[ key ] = time;
                return true;
            }
            // or compare with an older one
            auto &prev = mModificationTimes[ key ];
            if( prev < time ) {
                prev = time;
                return true;
            }
            return false;
        };
        
    protected:
        ci::fs::path                                mPath;
        std::string                                 mFilter;
        std::map< std::string, time_t >             mModificationTimes;
        std::atomic<bool>                           mWatching;
        std::unique_ptr<std::thread>                mThread;
        std::function<void(const ci::fs::path&)>    mCallback;
    };
    
    void watchImpl( const ci::fs::path &path, const std::function<void(const ci::fs::path&)> &callback )
    {
        // check if there's a wildcard in the path
        std::string key     = path.string();
        ci::fs::path p      = path;
        size_t wildCardPos  = key.find( "*" );
        std::string filter;
        if( wildCardPos != std::string::npos ){
            filter  = path.filename().string();
            p       = path.parent_path();
        }
        
        // add a new watcher
       if( callback ){
           
           // the file doesn't exist
           if( filter.empty() && !ci::fs::exists( p ) ){
               throw WatchedFileSystemExc( path );
           }
           else {
               size_t wildcardPos   = filter.find( "*" );
               std::string before   = filter.substr( 0, wildcardPos );
               std::string after    = filter.substr( wildcardPos + 1 );
               bool found           = false;
               ci::fs::directory_iterator end;
               for( ci::fs::directory_iterator it( p ); it != end; ++it ){
                   std::string current = it->path().string();
                   size_t beforePos    = current.find( before );
                   size_t afterPos     = current.find_last_of( after );
                   if( ( beforePos != std::string::npos || before.empty() )
                      && ( afterPos != std::string::npos || after.empty() ) ) {
                       found = true;
                       break;
                   }
               }
               if( !found ){
                   throw WatchedFileSystemExc( path );
               }
           }
           
           if( mFileWatchers.find( key ) == mFileWatchers.end() ){
               auto newWatcher = mFileWatchers.emplace( key, std::unique_ptr<Watcher>( new Watcher( p, filter, callback ) ) );
               if( newWatcher.second ){
                   newWatcher.first->second->start();
               }
            }
        }
        // if there is no callback that means that we are unwatching
        else {
            // if the path is empty we unwatch all files
            if( path.empty() ){
                for( auto it = mFileWatchers.begin(); it != mFileWatchers.end(); ) {
                    it = mFileWatchers.erase( it );
                }
            }
            // or the specified file
            else {
                auto watcher = mFileWatchers.find( key );
                if( watcher != mFileWatchers.end() ){
                    mFileWatchers.erase( watcher );
                }
            }
        }
    }
    std::map<std::string,std::unique_ptr<Watcher>> mFileWatchers;
};

//! this class is only used in release mode when WATCHDOG_ONLY_IN_DEBUG is defined
class SleepyWatchdog {
public:
    
    //! executes the callback once
    static void watch( const ci::fs::path &path, const std::function<void(const ci::fs::path&)> &callback )
    {
        callback( path );
    }
    //! does nothing
    static void unwatch( const ci::fs::path &path ) {}
    
    //! does nothing
    static void unwatchAll() {}
    
#ifdef CINDER_CINDER
    //! executes the callback once
    static void watchAsset( const ci::fs::path &assetPath, const std::function<void(const ci::fs::path&)> &callback )
    {
        callback( assetPath );
    }
    
    //! does nothing
    static void unwatchAsset( const ci::fs::path &assetPath ) {}
#endif
};

// defines the macro that allow to change the RELEASE/DEBUG behavior
#ifdef WATCHDOG_ONLY_IN_DEBUG
    #if defined(NDEBUG) || defined(_NDEBUG) || defined(RELEASE) || defined(MASTER) || defined(GOLD)
        #define wd SleepyWatchdog
    #else
        #define wd Watchdog
    #endif
#else
    typedef Watchdog wfs;
#endif
