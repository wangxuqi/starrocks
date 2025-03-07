// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/test/java/org/apache/doris/common/proc/BackendProcNodeTest.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.common.proc;

import com.google.common.collect.ImmutableMap;
import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.catalog.Catalog;
import com.starrocks.catalog.DiskInfo;
import com.starrocks.common.AnalysisException;
import com.starrocks.persist.EditLog;
import com.starrocks.system.Backend;
import mockit.Expectations;
import mockit.Mocked;
import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;

import java.util.Map;

public class BackendProcNodeTest {
    private Backend b1;
    @Mocked
    private Catalog catalog;
    @Mocked
    private EditLog editLog;

    @Before
    public void setUp() {
        new Expectations() {
            {
                editLog.logAddBackend((Backend) any);
                minTimes = 0;

                editLog.logDropBackend((Backend) any);
                minTimes = 0;

                editLog.logBackendStateChange((Backend) any);
                minTimes = 0;

                catalog.getNextId();
                minTimes = 0;
                result = 10000L;

                catalog.getEditLog();
                minTimes = 0;
                result = editLog;
            }
        };

        new Expectations(catalog) {
            {
                Catalog.getCurrentCatalog();
                minTimes = 0;
                result = catalog;
            }
        };

        b1 = new Backend(1000, "host1", 10000);
        b1.updateOnce(10001, 10003, 10005);
        Map<String, DiskInfo> disks = Maps.newHashMap();
        disks.put("/home/disk1", new DiskInfo("/home/disk1"));
        ImmutableMap<String, DiskInfo> immutableMap = ImmutableMap.copyOf(disks);
        b1.setDisks(immutableMap);
    }

    @After
    public void tearDown() {
    }

    @Test
    public void testResultNormal() throws AnalysisException {
        BackendProcNode node = new BackendProcNode(b1);
        ProcResult result;

        // fetch result
        result = node.fetchResult();
        Assert.assertNotNull(result);
        Assert.assertTrue(result instanceof BaseProcResult);

        Assert.assertTrue(result.getRows().size() >= 1);
        Assert.assertEquals(Lists.newArrayList("RootPath", "DataUsedCapacity", "OtherUsedCapacity", "AvailCapacity",
                "TotalCapacity", "TotalUsedPct", "State", "PathHash"), result.getColumnNames());
    }

}
