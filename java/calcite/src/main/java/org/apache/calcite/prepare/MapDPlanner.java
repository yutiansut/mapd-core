/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.apache.calcite.prepare;

import com.mapd.calcite.parser.MapDParserOptions;

import org.apache.calcite.config.CalciteConnectionConfig;
import org.apache.calcite.config.CalciteConnectionConfigImpl;
import org.apache.calcite.config.CalciteConnectionProperty;
import org.apache.calcite.jdbc.CalciteSchema;
import org.apache.calcite.plan.Context;
import org.apache.calcite.plan.hep.HepPlanner;
import org.apache.calcite.plan.hep.HepProgram;
import org.apache.calcite.rel.RelNode;
import org.apache.calcite.rel.RelRoot;
import org.apache.calcite.rel.core.RelFactories;
import org.apache.calcite.rel.rules.DynamicFilterJoinRule;
import org.apache.calcite.rel.rules.FilterJoinRule;
import org.apache.calcite.schema.SchemaPlus;
import org.apache.calcite.sql.SqlNode;
import org.apache.calcite.sql.advise.SqlAdvisor;
import org.apache.calcite.sql.validate.SqlConformanceEnum;
import org.apache.calcite.sql.validate.SqlMoniker;
import org.apache.calcite.tools.FrameworkConfig;
import org.apache.calcite.tools.RelConversionException;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;

/**
 * Customised version of the PlannerImpl for MapD.
 * Used to be a copy of PlannerImpl,
 * refactored now to use inheritance to minimize maintenance efforts.
 * Implementation of {@link org.apache.calcite.tools.Planner}.
 */
public class MapDPlanner extends PlannerImpl {
  FrameworkConfig config;
  private List<MapDParserOptions.FilterPushDownInfo> filterPushDownInfo =
          new ArrayList<>();

  public MapDPlanner(FrameworkConfig config) {
    super(config);
    this.config = config;
  }

  private static SchemaPlus rootSchema(SchemaPlus schema) {
    for (;;) {
      if (schema.getParentSchema() == null) {
        return schema;
      }
      schema = schema.getParentSchema();
    }
  }

  public static class CompletionResult {
    public List<SqlMoniker> hints;
    public String replaced;

    CompletionResult(final List<SqlMoniker> hints, final String replaced) {
      this.hints = hints;
      this.replaced = replaced;
    }
  }

  private CalciteCatalogReader createCatalogReader() {
    final SchemaPlus rootSchema = rootSchema(config.getDefaultSchema());
    final Context context = config.getContext();
    final CalciteConnectionConfig connectionConfig;

    if (context != null) {
      connectionConfig = context.unwrap(CalciteConnectionConfig.class);
    } else {
      Properties properties = new Properties();
      properties.setProperty(CalciteConnectionProperty.CASE_SENSITIVE.camelName(),
              String.valueOf(config.getParserConfig().caseSensitive()));
      connectionConfig = new CalciteConnectionConfigImpl(properties);
    }

    return new CalciteCatalogReader(CalciteSchema.from(rootSchema),
            CalciteSchema.from(config.getDefaultSchema()).path(null),
            getTypeFactory(),
            connectionConfig);
  }

  public void ready() {
    // need to call ready on the super class, but that method is marked private
    // circumventing via reflection for now
    try {
      Method readyMethod = getClass().getSuperclass().getDeclaredMethod("ready");
      readyMethod.setAccessible(true);
      readyMethod.invoke(this);
    } catch (InvocationTargetException e) {
      if (e.getCause() instanceof RuntimeException) {
        throw(RuntimeException) e.getCause();
      } else {
        throw new RuntimeException(e.getCause());
      }
    } catch (Exception e) {
      throw new RuntimeException(e);
    }
  }

  public CompletionResult getCompletionHints(
          final String sql, final int cursor, final List<String> visibleTables) {
    ready();
    MapDSqlAdvisorValidator advisor_validator = new MapDSqlAdvisorValidator(visibleTables,
            config.getOperatorTable(),
            createCatalogReader(),
            getTypeFactory(),
            SqlConformanceEnum.LENIENT);
    SqlAdvisor advisor = new MapDSqlAdvisor(advisor_validator);
    String[] replaced = new String[1];
    int adjusted_cursor = cursor < 0 ? sql.length() : cursor;
    java.util.List<SqlMoniker> hints =
            advisor.getCompletionHints(sql, adjusted_cursor, replaced);
    return new CompletionResult(hints, replaced[0]);
  }

  @Override
  public RelRoot rel(SqlNode sql) throws RelConversionException {
    RelRoot root = super.rel(sql);
    root = applyFilterPushdown(root);
    return root;
  }

  private RelRoot applyFilterPushdown(RelRoot root) {
    if (filterPushDownInfo.isEmpty()) {
      return root;
    }
    final DynamicFilterJoinRule dynamicFilterJoinRule = new DynamicFilterJoinRule(true,
            RelFactories.LOGICAL_BUILDER,
            FilterJoinRule.TRUE_PREDICATE,
            filterPushDownInfo);
    final HepProgram program =
            HepProgram.builder().addRuleInstance(dynamicFilterJoinRule).build();
    HepPlanner prePlanner = new HepPlanner(program);
    prePlanner.setRoot(root.rel);
    final RelNode rootRelNode = prePlanner.findBestExp();
    filterPushDownInfo.clear();
    return root.withRel(rootRelNode);
  }

  public void setFilterPushDownInfo(
          final List<MapDParserOptions.FilterPushDownInfo> filterPushDownInfo) {
    this.filterPushDownInfo = filterPushDownInfo;
  }
}

// End MapDPlanner.java
