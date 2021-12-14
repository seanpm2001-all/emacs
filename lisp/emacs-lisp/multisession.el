;;; multisession.el --- Multisession storage for variables  -*- lexical-binding: t; -*-

;; Copyright (C) 2021 Free Software Foundation, Inc.

;; This file is part of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;;

;;; Code:

(require 'cl-lib)
(require 'eieio)
(require 'sqlite)
(require 'url)

(defcustom multisession-storage 'files
  "Storage method for multisession variables.
Valid methods are `sqlite' and `files'."
  :type '(choice (const :tag "SQLite" sqlite)
                 (const :tag "Files" files))
  :version "29.1"
  :group 'files)

(defcustom multisession-directory (expand-file-name "multisession/"
                                                    user-emacs-directory)
  "Directory to store multisession variables."
  :type 'file
  :version "29.1"
  :group 'files)

;;;###autoload
(defmacro define-multisession-variable (name initial-value &optional doc
                                             &rest args)
  "Make NAME into a multisession variable initialized from INITIAL-VALUE.
DOC should be a doc string, and ARGS are keywords as applicable to
`make-multisession'."
  (declare (indent defun))
  `(defvar ,name
     (make-multisession :key ',name
                        :initial-value ,initial-value
                        ,@args)
     ,@(list doc)))

(cl-defstruct (multisession
               (:constructor nil)
               (:constructor multisession--create)
               (:conc-name multisession--))
  "A persistent variable that will live across Emacs invocations."
  key
  (initial-value nil)
  package
  (synchronized nil)
  ;; We need an "impossible" value for the unbound case.
  (cached-value (make-marker))
  (cached-sequence 0))

(cl-defun make-multisession (&key key initial-value package synchronized)
  "Create a multisession object."
  (unless key
    (error "No key for the multisession object"))
  (unless package
    (setq package (intern (replace-regexp-in-string "-.*" ""
                                                    (symbol-name key)))))
  (multisession--create
   :key key
   :synchronized synchronized
   :initial-value initial-value
   :package package))

(defun multisession-value (object)
  "Return the value of the multisession OBJECT."
  (if (or (null user-init-file)
          (not (sqlite-available-p)))
      ;; If we don't have storage, then just return the value from the
      ;; object.
      (if (markerp (multisession--cached-value object))
          (multisession--initial-value object)
        (multisession--cached-value object))
    ;; We have storage, so we update from storage.
    (multisession-backend-value multisession-storage object)))

(defun multisession--set-value (object value)
  (if (or (null user-init-file)
          (not (sqlite-available-p)))
      ;; We have no backend, so just store the value.
      (setf (multisession--cached-value object) value)
    ;; We have a backend.
    (multisession--backend-set-value multisession-storage object value)))

(gv-define-simple-setter multisession-value multisession--set-value)

;; SQLite Backend

(defvar multisession--db nil)

(defun multisession--ensure-db ()
  (unless multisession--db
    (let* ((file (expand-file-name "sqlite/multisession.sqlite"
                                   multisession-directory))
           (dir (file-name-directory file)))
      (unless (file-exists-p dir)
        (make-directory dir t))
      (setq multisession--db (sqlite-open file)))
    (with-sqlite-transaction multisession--db
      (unless (sqlite-select
               multisession--db
               "select name from sqlite_master where type = 'table' and name = 'multisession'")
        ;; Use a write-ahead-log (available since 2010), which makes
        ;; writes a lot faster.
        (sqlite-pragma multisession--db "journal_mode = WAL")
        (sqlite-pragma multisession--db "bsynchronous = NORMAL")
        ;; Tidy up the database automatically.
        (sqlite-pragma multisession--db "auto_vacuum = FULL")
        ;; Create the table.
        (sqlite-execute
         multisession--db
         "create table multisession (package text not null, key text not null, sequence number not null default 1, value text not null)")
        (sqlite-execute
         multisession--db
         "create unique index multisession_idx on multisession (package, key)")))))

(cl-defmethod multisession-backend-value ((_type (eql sqlite)) object)
  (multisession--ensure-db)
  (let ((id (list (symbol-name (multisession--package object))
                  (symbol-name (multisession--key object)))))
    (cond
     ;; We have no value yet; check the database.
     ((markerp (multisession--cached-value object))
      (let ((stored
             (car
              (sqlite-select
               multisession--db
               "select value, sequence from multisession where package = ? and key = ?"
               id))))
        (if stored
            (let ((value (car (read-from-string (car stored)))))
              (setf (multisession--cached-value object) value
                    (multisession--cached-sequence object) (cadr stored))
              value)
          ;; Nothing; return the initial value.
          (multisession--initial-value object))))
     ;; We have a value, but we want to update in case some other
     ;; Emacs instance has updated.
     ((multisession--synchronized object)
      (let ((stored
             (car
              (sqlite-select
               multisession--db
               "select value, sequence from multisession where sequence > ? and package = ? and key = ?"
               (cons (multisession--cached-sequence object) id)))))
        (if stored
            (let ((value (car (read-from-string (car stored)))))
              (setf (multisession--cached-value object) value
                    (multisession--cached-sequence object) (cadr stored))
              value)
          ;; Nothing, return the cached value.
          (multisession--cached-value object))))
     ;; Just return the cached value.
     (t
      (multisession--cached-value object)))))

(cl-defmethod multisession--backend-set-value ((_type (eql sqlite))
                                               object value)
  (catch 'done
    (let ((i 0))
      (while (< i 10)
        (condition-case nil
            (throw 'done (multisession--set-value-sqlite object value))
          (sqlite-locked-error
           (setq i (1+ i))
           (sleep-for (+ 0.1 (/ (float (random 10)) 10))))))
      (signal 'sqlite-locked-error "Database is locked"))))

(defun multisession--set-value-sqlite (object value)
  (multisession--ensure-db)
  (with-sqlite-transaction multisession--db
    (let ((id (list (symbol-name (multisession--package object))
                    (symbol-name (multisession--key object))))
          (pvalue
           (let ((print-length nil)
                 (print-circle t)
                 (print-level nil))
             (prin1-to-string value))))
      (sqlite-execute
       multisession--db
       "insert into multisession(package, key, sequence, value) values(?, ?, 1, ?) on conflict(package, key) do update set sequence = sequence + 1, value = ?"
       (append id (list pvalue pvalue)))
      (setf (multisession--cached-sequence object)
            (caar (sqlite-select
                   multisession--db
                   "select sequence from multisession where package = ? and key = ?"
                   id)))
      (setf (multisession--cached-value object) value))))

(cl-defmethod multisession--backend-values ((_type (eql sqlite)))
  (sqlite-select
   multisession--db
   "select package, key, value from multisession order by package, key"))

(cl-defmethod multisession--backend-delete ((_type (eql sqlite)) id)
  (sqlite-execute multisession--db
                  "delete from multisession where package = ? and key = ?"
                  id))

;; Files Backend

(defun multisession--encode-file-name (name)
  (url-hexify-string name))

(defun multisession--update-file-value (file object)
  (with-temp-buffer
    (let* ((time (file-attribute-modification-time
                  (file-attributes file)))
           (coding-system-for-read 'utf-8))
      (insert-file-contents file)
      (let ((stored (read (current-buffer))))
        (setf (multisession--cached-value object) stored
              (multisession--cached-sequence object) time)
        stored))))

(defun multisession--object-file-name (object)
  (expand-file-name
   (concat "files/"
           (multisession--encode-file-name
            (symbol-name (multisession--package object)))
           "/"
           (multisession--encode-file-name
            (symbol-name (multisession--key object)))
           ".value")
   multisession-directory))

(cl-defmethod multisession-backend-value ((_type (eql files)) object)
  (let ((file (multisession--object-file-name object)))
    (cond
     ;; We have no value yet; see whether it's stored.
     ((markerp (multisession--cached-value object))
      (if (file-exists-p file)
          (multisession--update-file-value file object)
        ;; Nope; return the initial value.
        (multisession--initial-value object)))
     ;; We have a value, but we want to update in case some other
     ;; Emacs instance has updated.
     ((multisession--synchronized object)
      (if (and (file-exists-p file)
               (time-less-p (multisession--cached-sequence object)
                            (file-attribute-modification-time
                             (file-attributes file))))
          (multisession--update-file-value file object)
        ;; Nothing, return the cached value.
        (multisession--cached-value object)))
     ;; Just return the cached value.
     (t
      (multisession--cached-value object)))))

(cl-defmethod multisession--backend-set-value ((_type (eql files))
                                               object value)
  (let ((file (multisession--object-file-name object))
        (time (current-time)))
    ;; Ensure that the directory exists.
    (let ((dir (file-name-directory file)))
      (unless (file-exists-p dir)
        (make-directory dir t)))
    (with-temp-buffer
      (let ((print-length nil)
            (print-circle t)
            (print-level nil))
        (prin1 value (current-buffer)))
      (let ((coding-system-for-write 'utf-8)
            (create-lockfiles nil))
        (write-region (point-min) (point-max) file nil 'silent)))
    (setf (multisession--cached-sequence object) time
          (multisession--cached-value object) value)))

(cl-defmethod multisession--backend-values ((_type (eql files)))
  (mapcar (lambda (file)
            (let ((bits (file-name-split file)))
              (list (url-unhex-string (car (last bits 2)))
                    (url-unhex-string
                     (file-name-sans-extension (car (last bits))))
                    (with-temp-buffer
                      (let ((coding-system-for-read 'utf-8))
                        (insert-file-contents file)
                        (read (current-buffer)))))))
          (directory-files-recursively
           (expand-file-name "files" multisession-directory)
           "\\.value\\'")))

(cl-defmethod multisession--backend-delete ((_type (eql files)) id)
  (let ((file (multisession--object-file-name
               (make-multisession :package (intern (car id))
                                  :key (intern (cadr id))))))
    (when (file-exists-p file)
      (delete-file file))))

;; (define-multisession-variable foo 'bar)
;; (multisession-value foo)
;; (multisession--set-value foo 'zot)
;; (setf (multisession-value foo) 'gazonk)

;; Mode for editing.

(defvar-keymap multisession-edit-mode-map
  "d" #'multisession-delete-value)

(define-derived-mode multisession-edit-mode special-mode "Multisession"
  "This mode lists all elements in the \"multisession\" database."
  :interactive nil
  (buffer-disable-undo)
  (setq-local buffer-read-only t))

;;;###autoload
(defun list-multisession-values ()
  "List all values in the \"multisession\" database."
  (interactive)
  (multisession--ensure-db)
  (pop-to-buffer (get-buffer-create "*Multisession*"))
  (let ((inhibit-read-only t))
    (erase-buffer)
    (cl-loop for (package key value)
             in (multisession--backend-values multisession-storage)
             do (insert (propertize (format "%s %s %s\n"
                                            package key value)
                                    'multisession--id (list package key))))
    (goto-char (point-min)))
  (multisession-edit-mode))

(defun multisession-delete-value (id)
  "Delete the value at point."
  (interactive (list (get-text-property (point) 'multisession--id))
               multisession-edit-mode)
  (unless id
    (error "No value on the current line"))
  (multisession--backend-delete multisession-storage id)
  (let ((inhibit-read-only t))
    (beginning-of-line)
    (delete-region (point) (progn (forward-line 1) (point)))))

(provide 'multisession)

;;; multisession.el ends here
