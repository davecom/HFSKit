// HFSKit.swift
// Swift wrapper around hfswrapper.h / hfswrapper.c.
//
// Usage in SwiftPM:
// - C target "HFSCore" builds hfswrapper.c and links against libhfs.
// - Swift target "HFSKit" depends on "HFSCore" and imports it.
//
// Usage example:
//
//   import HFSKit
//
//   let vol = try HFSVolume(path: URL(fileURLWithPath: "/path/to/image.dsk"),
//                           writable: true)
//   let entries = try vol.list(directory: ":System Folder")
//   try vol.copyIn(hostPath: URL(fileURLWithPath: "/tmp/foo"),
//                  toHFSPath: ":foo")
//   try vol.copyOut(hfsPath: ":System Folder:Finder",
//                   toHostPath: URL(fileURLWithPath: "/tmp/Finder"))
//

import Foundation
import HFSCore   // This is the Clang module for your C target (containing hfswrapper.h/c)

// MARK: - Errors

public enum HFSError: Error {
    case openFailed
    case operationFailed(String? = nil)
}

// MARK: - File info

public struct HFSFileInfo: CustomStringConvertible {
    public let name: String
    public let path: String
    public let isDirectory: Bool
    public let dataForkSize: Int
    public let resourceForkSize: Int
    public let fileType: String
    public let fileCreator: String
    public let flags: UInt16
    public let created: Date
    public let modified: Date

    public var description: String {
        return "HFSFileInfo(name: \(name), path: \(path), dir: \(isDirectory), data: \(dataForkSize), rsrc: \(resourceForkSize))"
    }
}

public struct HFSVolumeInfo: CustomStringConvertible {
    public let name: String
    public let flags: UInt32
    public let totalBytes: UInt64
    public let freeBytes: UInt64
    public let allocationBlockSize: UInt32
    public let clumpSize: UInt32
    public let numberOfFiles: UInt32
    public let numberOfDirectories: UInt32
    public let created: Date
    public let modified: Date
    public let backup: Date
    public let blessedFolderId: UInt32

    public var usedBytes: UInt64 {
        return totalBytes - freeBytes
    }

    public var description: String {
        return "HFSVolumeInfo(name: \(name), total: \(totalBytes), free: \(freeBytes))"
    }
}

// MARK: - Volume

public final class HFSVolume {
    private var handle: UnsafeMutablePointer<HFSImage>?

    public var isClosed: Bool {
        return handle == nil
    }

    public init(path: URL, writable: Bool) throws {
        let cPath = path.path.cString(using: .utf8)!
        let rw = writable ? 1 : 0

        guard let h = hfsw_open_image(cPath, Int32(rw)) else {
            throw HFSError.openFailed
        }
        self.handle = h
    }

    deinit {
        close()
    }

    public func close() {
        if let h = handle {
            hfsw_close_image(h)
            handle = nil
        }
    }

    private func requireHandle() throws -> UnsafeMutablePointer<HFSImage> {
        guard let h = handle else {
            throw HFSError.operationFailed("Volume is closed")
        }
        return h
    }

    // MARK: - Stat

    public func stat(path: String) throws -> HFSFileInfo {
        let h = try requireHandle()

        var cInfo = HFSWFileInfo()
        let status = path.withCString { cPath in
            hfsw_stat(h, cPath, &cInfo)
        }

        guard status == 0 else {
            throw HFSError.operationFailed("stat failed for \(path)")
        }

        return HFSFileInfo(from: cInfo, path: path)
    }

    public func attributes(of path: String) throws -> HFSFileInfo {
        return try stat(path: path)
    }

    // MARK: - Directory listing

    private final class ListContext {
        var items: [HFSFileInfo] = []
        var basePath: String = ":"
    }

    public func list(directory hfsPath: String = ":") throws -> [HFSFileInfo] {
        let h = try requireHandle()
        let context = ListContext()
        let ctxPtr = Unmanaged.passUnretained(context).toOpaque()

        context.basePath = hfsPath.isEmpty ? ":" : hfsPath
        let callback: hfsw_list_callback = { infoPtr, rawCtx in
            guard let infoPtr = infoPtr,
                  let rawCtx = rawCtx else { return }

            let ctx = Unmanaged<ListContext>
                .fromOpaque(rawCtx)
                .takeUnretainedValue()

            let cInfo = infoPtr.pointee
            let name = stringFromFixedArray(cInfo.name)
            let itemPath = joinHFSPath(ctx.basePath, name)
            let swiftInfo = HFSFileInfo(from: cInfo, path: itemPath)
            ctx.items.append(swiftInfo)
        }

        let status = context.basePath.withCString { cPath in
            hfsw_list_dir(h, cPath, callback, ctxPtr)
        }

        guard status == 0 else {
            throw HFSError.operationFailed("list failed for \(hfsPath)")
        }

        return context.items
    }

    // MARK: - Volume info

    public func volumeInfo() throws -> HFSVolumeInfo {
        let h = try requireHandle()

        var cInfo = HFSWVolumeInfo()
        let status = hfsw_volume_info(h, &cInfo)
        guard status == 0 else {
            throw HFSError.operationFailed("volume info failed")
        }

        return HFSVolumeInfo(from: cInfo)
    }

    // MARK: - Basic operations

    public func delete(path: String) throws {
        let h = try requireHandle()
        let status = path.withCString { cPath in
            hfsw_delete(h, cPath)
        }
        guard status == 0 else {
            throw HFSError.operationFailed("delete failed for \(path)")
        }
    }

    public func delete(_ info: HFSFileInfo) throws {
        if info.isDirectory {
            try deleteDirectory(info)
        } else {
            try delete(path: info.path)
        }
    }

    public func rename(path: String, to newName: String) throws {
        let h = try requireHandle()
        let status = path.withCString { cOld in
            newName.withCString { cNew in
                hfsw_rename(h, cOld, cNew)
            }
        }
        guard status == 0 else {
            throw HFSError.operationFailed("rename failed for \(path) -> \(newName)")
        }
    }

    public func rename(_ info: HFSFileInfo, to newName: String) throws {
        try rename(path: info.path, to: newName)
    }

    public func makeDirectory(path: String) throws {
        let h = try requireHandle()
        let status = path.withCString { cPath in
            hfsw_mkdir(h, cPath)
        }
        guard status == 0 else {
            throw HFSError.operationFailed("mkdir failed for \(path)")
        }
    }

    public func makeDirectory(_ info: HFSFileInfo) throws {
        try makeDirectory(path: info.path)
    }

    // MARK: - Copy in/out

    public enum CopyMode: Int32 {
        case auto = 0
        case raw  = 1
    }

    public func copyIn(hostPath: URL,
                       toHFSPath hfsPath: String,
                       mode: CopyMode = .auto) throws
    {
        let h = try requireHandle()

        let status = hostPath.path.withCString { cHost in
            hfsPath.withCString { cHFS in
                hfsw_copy_in(h, cHost, cHFS, mode.rawValue)
            }
        }

        guard status == 0 else {
            throw HFSError.operationFailed("copyIn failed for \(hostPath.path) -> \(hfsPath)")
        }
    }

    public func copyIn(hostPath: URL,
                       toHFSPath info: HFSFileInfo,
                       mode: CopyMode = .auto) throws
    {
        try copyIn(hostPath: hostPath, toHFSPath: info.path, mode: mode)
    }

    public func copyOut(hfsPath: String,
                        toHostPath hostPath: URL,
                        mode: CopyMode = .auto) throws
    {
        let h = try requireHandle()

        let status = hfsPath.withCString { cHFS in
            hostPath.path.withCString { cHost in
                hfsw_copy_out(h, cHFS, cHost, mode.rawValue)
            }
        }

        guard status == 0 else {
            throw HFSError.operationFailed("copyOut failed for \(hfsPath) -> \(hostPath.path)")
        }
    }

    public func copyOut(hfsPath info: HFSFileInfo,
                        toHostPath hostPath: URL,
                        mode: CopyMode = .auto) throws
    {
        if info.isDirectory {
            try copyOutDirectory(hfsPath: info.path, toHostDirectory: hostPath, mode: mode)
        } else {
            try copyOut(hfsPath: info.path, toHostPath: hostPath, mode: mode)
        }
    }

    public func copyInDirectory(hostDirectory: URL,
                                toHFSPath hfsPath: String,
                                mode: CopyMode = .auto) throws
    {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: hostDirectory.path, isDirectory: &isDir), isDir.boolValue else {
            throw HFSError.operationFailed("copyInDirectory source is not a directory: \(hostDirectory.path)")
        }

        try ensureDirectoryExists(at: hfsPath)

        let baseURL = hostDirectory.resolvingSymlinksInPath()
        let baseComponents = baseURL.pathComponents

        let enumerator = fm.enumerator(at: baseURL, includingPropertiesForKeys: [.isDirectoryKey], options: [])
        while let item = enumerator?.nextObject() as? URL {
            let itemURL = item.resolvingSymlinksInPath()
            let itemComponents = itemURL.pathComponents
            let relComponents = Array(itemComponents.dropFirst(baseComponents.count))
            let destPath = relComponents.reduce(hfsPath) { current, component in
                joinHFSPath(current, component)
            }

            let resourceValues = try item.resourceValues(forKeys: [.isDirectoryKey])
            if resourceValues.isDirectory == true {
                try ensureDirectoryExists(at: destPath)
            } else {
                try copyIn(hostPath: item, toHFSPath: destPath, mode: mode)
            }
        }
    }

    public func copyOutDirectory(hfsPath: String,
                                 toHostDirectory hostDirectory: URL,
                                 mode: CopyMode = .auto) throws
    {
        let fm = FileManager.default
        try fm.createDirectory(at: hostDirectory, withIntermediateDirectories: true)

        for entry in try list(directory: hfsPath) {
            let entryHFSPath = joinHFSPath(hfsPath, entry.name)
            let entryHostURL = hostDirectory.appendingPathComponent(entry.name)
            if entry.isDirectory {
                try copyOutDirectory(hfsPath: entryHFSPath, toHostDirectory: entryHostURL, mode: mode)
            } else {
                try copyOut(hfsPath: entryHFSPath, toHostPath: entryHostURL, mode: mode)
            }
        }
    }

    // MARK: - Type/creator

    public func setTypeCreator(path: String,
                               fileType: String,
                               fileCreator: String) throws
    {
        let h = try requireHandle()

        let status = path.withCString { cPath in
            fileType.withCString { cType in
                fileCreator.withCString { cCreator in
                    hfsw_set_type_creator(h, cPath, cType, cCreator)
                }
            }
        }

        guard status == 0 else {
            throw HFSError.operationFailed("setTypeCreator failed for \(path)")
        }
    }

    public func setTypeCreator(path info: HFSFileInfo,
                               fileType: String,
                               fileCreator: String) throws
    {
        try setTypeCreator(path: info.path, fileType: fileType, fileCreator: fileCreator)
    }

    // MARK: - Directory delete

    public func deleteDirectory(path: String) throws {
        let info = try stat(path: path)
        if info.isDirectory {
            for entry in try list(directory: path) {
                let entryPath = joinHFSPath(path, entry.name)
                if entry.isDirectory {
                    try deleteDirectory(path: entryPath)
                } else {
                    try delete(path: entryPath)
                }
            }
        }
        try delete(path: path)
    }

    public func deleteDirectory(_ info: HFSFileInfo) throws {
        try deleteDirectory(path: info.path)
    }

    private func ensureDirectoryExists(at hfsPath: String) throws {
        do {
            let info = try stat(path: hfsPath)
            if !info.isDirectory {
                throw HFSError.operationFailed("Path exists and is not a directory: \(hfsPath)")
            }
        } catch {
            try makeDirectory(path: hfsPath)
        }
    }
}

// MARK: - Internal conversion

private extension HFSFileInfo {
    init(from cInfo: HFSWFileInfo, path: String) {
        let name = stringFromFixedArray(cInfo.name)

        let typeStr = stringFromFixedArray(cInfo.fileType)
        let creatorStr = stringFromFixedArray(cInfo.fileCreator)

        self.init(
            name: name,
            path: path,
            isDirectory: cInfo.isDirectory != 0,
            dataForkSize: Int(cInfo.dataForkSize),
            resourceForkSize: Int(cInfo.rsrcForkSize),
            fileType: typeStr,
            fileCreator: creatorStr,
            flags: cInfo.flags,
            created: Date(timeIntervalSince1970: TimeInterval(cInfo.created)),
            modified: Date(timeIntervalSince1970: TimeInterval(cInfo.modified))
        )
    }
}

private extension HFSVolumeInfo {
    init(from cInfo: HFSWVolumeInfo) {
        self.init(
            name: stringFromFixedArray(cInfo.name),
            flags: cInfo.flags,
            totalBytes: cInfo.totalBytes,
            freeBytes: cInfo.freeBytes,
            allocationBlockSize: cInfo.allocationBlockSize,
            clumpSize: cInfo.clumpSize,
            numberOfFiles: cInfo.numberOfFiles,
            numberOfDirectories: cInfo.numberOfDirectories,
            created: Date(timeIntervalSince1970: TimeInterval(cInfo.created)),
            modified: Date(timeIntervalSince1970: TimeInterval(cInfo.modified)),
            backup: Date(timeIntervalSince1970: TimeInterval(cInfo.backup)),
            blessedFolderId: cInfo.blessedFolderId
        )
    }
}

private func stringFromFixedArray<T>(_ array: T) -> String {
    return withUnsafePointer(to: array) { ptr in
        ptr.withMemoryRebound(to: CChar.self, capacity: MemoryLayout.size(ofValue: array)) { cPtr in
            String(cString: cPtr)
        }
    }
}

private func joinHFSPath(_ base: String, _ name: String) -> String {
    if base == ":" { return ":\(name)" }
    if base.hasSuffix(":") { return base + name }
    return base + ":\(name)"
}
