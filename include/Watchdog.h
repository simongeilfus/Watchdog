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

#pragma once

#include <map>
#include <string>
#include <thread>
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
class WatchedFileSystemExc : public std::exception {
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
        // create the static instance Watchdog instance
        static Watchdog wd;
        // and start its thread
        if( !wd.mWatching ) wd.start();
        
        wd.watchImpl( path, callback );
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
    //! sets the last modification time of a file or directory. by default sets the time to the current time
    static void touch( const ci::fs::path &path, std::time_t time = std::time( nullptr ) )
    {
        ci::fs::last_write_time( path, time );
    }
    
#ifdef CINDER_CINDER
    //! watches an asset for modification and call back the specified std::function
    static void watchAsset( const ci::fs::path &assetPath, const std::function<void(const ci::fs::path&)> &callback )
    {
        watch( ci::app::getAssetPath( "" ) / assetPath, callback );
    }
    //! unwatches a previously registrated asset
    static void unwatchAsset( const ci::fs::path &assetPath )
    {
        watch( ci::app::getAssetPath( "" ) / assetPath, std::function<void(const ci::fs::path&)>() );
    }
    //! sets the last modification time of an asset. by default sets the time to the current time
    static void touchAsset( const ci::fs::path &assetPath, std::time_t time = std::time( nullptr ) )
    {
        ci::fs::last_write_time( ci::app::getAssetPath("") / assetPath, time );
    }
#endif
    
protected:
    
    Watchdog()
    : mWatching(false)
    {
    }
    
    ~Watchdog()
    {
        // remove all watchers
        unwatchAll();
        
        // stop the thread
        mWatching = false;
        if( mThread->joinable() ) mThread->join();
    }
    
    
    class Watcher {
    public:
        Watcher( const ci::fs::path &path, const std::string &filter, const std::function<void(const ci::fs::path&)> &callback )
        : mPath(path), mFilter(filter), mCallback(callback)
        {
            // make sure we store all initial write time
            if( !mFilter.empty() ) {
                size_t wildcardPos  = mFilter.find( "*" );
                if( wildcardPos != std::string::npos ) {
                    std::string before  = mFilter.substr( 0, wildcardPos );
                    std::string after   = mFilter.substr( wildcardPos + 1 );
                    ci::fs::directory_iterator end;
                    for( ci::fs::directory_iterator it( mPath ); it != end; ++it ){
                        std::string p       = it->path().string();
                        size_t beforePos    = p.find( before );
                        size_t afterPos     = p.find( after );
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
        }
        
        void watch()
        {
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
                            size_t afterPos     = p.find( after );
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
        }
        
        bool hasChanged( const ci::fs::path &path )
        {
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
        std::function<void(const ci::fs::path&)>    mCallback;
        std::map< std::string, time_t >             mModificationTimes;
    };
    
    
    void start()
    {
        mWatching   = true;
        mThread     = std::unique_ptr<std::thread>( new std::thread( [this](){
            // keep watching for modifications every ms milliseconds
            auto ms = std::chrono::milliseconds( 500 );
            while( mWatching ) {
                // iterate through each watcher and check for modification
                std::lock_guard<std::mutex> lock( mMutex );
                auto end = mFileWatchers.end();
                for( auto it = mFileWatchers.begin(); it != end; ++it ) {
                    it->second.watch();
                }
                
                // make this thread sleep for a while
                std::this_thread::sleep_for( ms );
            }
        } ) );
    }
    
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
           
           // throw an exception if the file doesn't exist
           if( filter.empty() && !ci::fs::exists( p ) ){
               throw WatchedFileSystemExc( path );
           }
           else if( !filter.empty() ) {
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
           
           std::lock_guard<std::mutex> lock( mMutex );
           if( mFileWatchers.find( key ) == mFileWatchers.end() ){
               mFileWatchers.emplace( make_pair( key, Watcher( p, filter, callback ) ) );
           }
        }
        // if there is no callback that means that we are unwatching
        else {
            // if the path is empty we unwatch all files
            if( path.empty() ){
                std::lock_guard<std::mutex> lock( mMutex );
                for( auto it = mFileWatchers.begin(); it != mFileWatchers.end(); ) {
                    it = mFileWatchers.erase( it );
                }
            }
            // or the specified file
            else {
                std::lock_guard<std::mutex> lock( mMutex );
                auto watcher = mFileWatchers.find( key );
                if( watcher != mFileWatchers.end() ){
                    mFileWatchers.erase( watcher );
                }
            }
        }
    }
    
    std::mutex                      mMutex;
    std::atomic<bool>               mWatching;
    std::unique_ptr<std::thread>    mThread;
    std::map<std::string,Watcher>   mFileWatchers;
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
