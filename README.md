
# FUSE 기반 블록 파일 시스템 구현 (C)

## 1. 프로젝트 개요

C 언어와 FUSE(Filesystem in Userspace)를 활용하여 사용자 공간에서 동작하는 블록 기반 파일 시스템을 구현한 프로젝트입니다.
디스크 이미지 위에 파일 시스템 구조를 직접 설계하고, 파일 및 디렉토리의 생성, 삭제, 읽기/쓰기, 조회 기능을 구현하였습니다.

구현은 다음 두 구성 요소로 이루어집니다:

* **mkfs**: 디스크 이미지를 파일 시스템 구조로 초기화
* **wfs**: FUSE를 통해 파일 시스템을 마운트하고 동작 수행

---

## 2. 파일 시스템 구조

파일 시스템은 디스크 이미지 위에 다음과 같은 구조로 구성됩니다:

```
[ Superblock | Inode Bitmap | Data Bitmap | Inode Blocks | Data Blocks ]
```

* **Superblock**: 파일 시스템 전체 메타데이터 관리
* **Inode Bitmap**: inode 할당 상태 관리
* **Data Bitmap**: 데이터 블록 할당 상태 관리
* **Inode Blocks**: 파일 및 디렉토리 메타데이터 저장
* **Data Blocks**: 실제 파일 데이터 및 디렉토리 엔트리 저장

각 파일은 inode를 통해 관리되며, direct block과 single indirect block을 통해 데이터를 참조하도록 설계하였습니다.

---

## 3. 주요 구현 내용

### 3.1 파일 시스템 초기화 (mkfs)

* inode 및 데이터 블록 수를 32 단위로 정렬하여 관리
* 디스크 내 각 영역의 오프셋 계산
* superblock, bitmap, root inode 초기화
* `mmap`을 활용하여 디스크 이미지에 직접 접근 및 초기화

---

### 3.2 경로 탐색 (Path Resolution)

* `/a/b/c` 형태의 경로를 파싱하여 디렉토리 엔트리를 순회
* target inode 및 parent inode를 탐색하는 로직 구현

---

### 3.3 파일 및 디렉토리 생성

* bitmap 기반 free inode 탐색 및 할당
* 부모 디렉토리에 directory entry 추가
* inode 메타데이터 초기화

---

### 3.4 삭제 처리

* 데이터 블록 해제 및 inode bitmap 갱신
* 부모 디렉토리에서 entry 제거
* 디렉토리는 비어있는 경우에만 삭제 가능하도록 처리

---

### 3.5 읽기 / 쓰기 (Read / Write)

* 파일 offset 기반으로 block 위치 계산
* 여러 block에 걸친 read/write 처리
* 파일 크기 증가 시 block 동적 할당
* direct block과 indirect block을 활용한 데이터 관리

---

### 3.6 디렉토리 조회

* 디렉토리 엔트리를 순회하여 파일 목록 반환 (`readdir`)
* `ls` 명령어 동작 가능하도록 구현

---

## 4. 할당 및 관리 전략

* **Inode**: bitmap 기반으로 free inode 탐색 및 할당
* **Data Block**: bitmap 기반 allocation, 삭제 시 block 반환
* 파일 크기 증가 시 동적으로 block을 확장하도록 구현

---

## 5. FUSE 기반 구현

FUSE를 활용하여 커널 수정 없이 사용자 공간에서 파일 시스템을 구현하였습니다.

구현된 주요 operation:

* getattr
* mknod
* mkdir
* unlink
* rmdir
* read
* write
* readdir

이를 통해 `ls`, `cat`, `mkdir` 등의 명령어가 정상적으로 동작하도록 구현하였습니다.

---

## 6. 기술적 도전 과제

* 경로 문자열을 inode 구조로 변환하는 path traversal 구현
* block 단위 저장 구조에서 offset 기반 read/write 처리
* indirect block을 활용한 파일 확장 처리
* bitmap, inode, directory entry 간 데이터 일관성 유지

---

## 7. 프로젝트를 통해 배운 점

* inode와 data block 기반 파일 시스템 구조 이해
* 디렉토리 구조와 실제 저장 방식 간의 연결 과정 이해
* read/write가 block 단위로 처리되는 내부 동작 이해
* 사용자 공간에서 파일 시스템을 구현하는 방식 경험

---

## 8. 결론

본 프로젝트를 통해 파일 시스템의 저장 구조부터 입출력 처리까지 핵심 동작을 직접 구현하였으며,
운영체제의 파일 관리 구조를 코드 수준에서 이해할 수 있었습니다.

