# XContents
A minimal fileserver over http. 

XContents is a minimal fileserver over http.
It reads and writes files only in the directory where is is started, and its subdirectories.
If the requested resource ends with /, `index.html` is added. There is no directory listing.

## Methods
   The following methods are supported.
   - GET: If the specified resource exists, it is returned.
   - HEAD: Like GET, except only the headers are returned, with no content.
   - PUT: The file is created or overwritten.
   - POST: 
      If the resource does not exist, it is created, with the content surrounded by the tags `<post>...</post>`.
      If it exists, the content is appended before the last closing tag, if there is one, and otherwise at the end.
      Therefore, if you don't like the tags `<post>...</post>` when the file is created, 
         first `PUT` an empty file, or one containing only the open and closing tags, e.g. `<data></data>`,
         before you `POST` to it.
   - OPTIONS: Access control allows access from anywhere.

## Errors
   - 404: File not found for GET or HEAD
   - 403: No access to directories (for reading or writing)
   - 403: If file can't be opened for writing for a PUT or POST
   - 400: Bad request, if the URI contains "/../" (Most browsers pre-process ".." path steps, so this should never arise.)
   - 501: Not implemented, for other methods.

   Mostly not robust or secure. 
   For demonstration and test purposes only: do not use for production.
   Based on tiny.c by Dave O'Hallaron, Carnegie Mellon.

   compile: `cc xcontents.c -o xcontents`

   usage:   `xcontents <port>`
