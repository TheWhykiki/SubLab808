import Foundation

final class HTTPClient: NSObject, URLSessionDataDelegate, URLSessionDownloadDelegate {
    private var session: URLSession!
    private var task: URLSessionTask?
    private var data = Data()
    private var redirects = 0
    private var finished = false
    private let limit: Int
    private let destination: URL?
    private let progress: (Double) -> Void
    private let completion: (Result<Data, Error>) -> Void
    private let metadata: Bool

    init(url: URL, destination: URL? = nil, limit: Int,
         progress: @escaping (Double) -> Void = { _ in }, completion: @escaping (Result<Data, Error>) -> Void) {
        self.limit = limit
        self.destination = destination
        self.progress = progress
        self.completion = completion
        metadata = destination == nil
        super.init()
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 25
        configuration.timeoutIntervalForResource = 300
        configuration.httpShouldSetCookies = false
        configuration.urlCache = nil
        let queue = OperationQueue()
        queue.maxConcurrentOperationCount = 1
        session = URLSession(configuration: configuration, delegate: self, delegateQueue: queue)
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData)
        request.setValue("Whykiki-Plugin-Updater/1", forHTTPHeaderField: "User-Agent")
        if metadata {
            request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            request.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
        }
        task = metadata ? session.dataTask(with: request) : session.downloadTask(with: request)
        task?.resume()
    }

    func cancel() { task?.cancel() }

    private func finish(_ result: Result<Data, Error>) {
        guard !finished else { return }
        finished = true
        session.invalidateAndCancel()
        DispatchQueue.main.async { self.completion(result) }
    }

    private func check(_ response: URLResponse) throws {
        guard let response = response as? HTTPURLResponse else { throw UpdateFailure("Ungültige Serverantwort") }
        if response.statusCode == 404 { throw UpdateFailure("Für dieses Plugin ist noch kein stabiles Update verfügbar.") }
        if response.statusCode == 403 || response.statusCode == 429 { throw UpdateFailure("GitHub begrenzt die Anfragen. Bitte später erneut versuchen.") }
        try require(response.statusCode == 200, "Download fehlgeschlagen (HTTP \(response.statusCode))")
        try require(response.expectedContentLength <= Int64(limit), "Die Serverantwort ist zu groß")
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        redirects += 1
        let allowed = metadata ? ["api.github.com"] : ["github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"]
        guard let url = request.url, url.scheme == "https", allowed.contains(url.host ?? ""),
              url.user == nil, url.password == nil, url.port == nil || url.port == 443, redirects <= 5 else {
            completionHandler(nil)
            finish(.failure(UpdateFailure("Nicht vertrauenswürdige Download-Weiterleitung")))
            return
        }
        completionHandler(request)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        do { try check(response); completionHandler(.allow) }
        catch { completionHandler(.cancel); finish(.failure(error)) }
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive chunk: Data) {
        guard data.count <= limit - chunk.count else {
            finish(.failure(UpdateFailure("Die Release-Antwort ist zu groß")))
            return
        }
        data.append(chunk)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard totalBytesWritten <= Int64(limit) else {
            finish(.failure(UpdateFailure("Der Download überschreitet die erlaubte Größe")))
            return
        }
        DispatchQueue.main.async { self.progress(Double(totalBytesWritten) / Double(self.limit)) }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        do {
            guard let response = downloadTask.response, let destination = destination else { throw UpdateFailure("Unvollständiger Download") }
            try check(response)
            let size = try location.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
            try require(size > 0 && size <= limit, "Ungültige Downloadgröße")
            try FileManager.default.moveItem(at: location, to: destination)
            finish(.success(Data()))
        } catch { finish(.failure(error)) }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error = error { finish(.failure(error)) }
        else if metadata { finish(.success(data)) }
    }
}
